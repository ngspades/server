#ifndef PLAYERSTRUCT_H
#define PLAYERSTRUCT_H

#include <Server/Structs/CommandStruct.h>
#include <Server/Structs/GrenadeStruct.h>
#include <Server/Structs/IPStruct.h>
#include <Server/Structs/MapStruct.h>
#include <Server/Structs/MovementStruct.h>
#include <Server/Structs/TimerStruct.h>
#include <Util/Enums.h>
#include <Util/Queue.h>
#include <Util/Types.h>
#include <uthash/uthash.h>
#include <Server/Net/Conn.h>
#include <proto10/codec.h>
#include <stdint.h>

#define PLAYER_NAME_STRLEN 16

/* one sample of a body for lag compensation (Server/LagComp.c): 64 at 60 Hz
   are about one second, the most a shot is rewound */
#define LAG_HISTORY_SIZE 64
typedef struct lag_sample
{
    uint64_t   at; /* get_nanos() */
    vector3f_t pos;
    uint8_t    alive;
} lag_sample_t;

/* one damage taken, for kill assists (Assists.c) */
#define DAMAGE_LOG_SIZE 8
typedef struct damage_log
{
    uint8_t  attacker;
    uint16_t amount;
    uint64_t at; /* get_nanos() */
} damage_log_t;

/* one kind of input's budget (Server/Flood.c) */
typedef struct flood_bucket
{
    float    tokens;
    uint64_t at;        /* get_real_nanos() of the last input */
    uint64_t window_at; /* when the drops being counted began */
    unsigned dropped;
} flood_bucket_t;

typedef struct player
{
    UT_hash_handle           hh;
    queue_t*                 map_queue;
    conn_t*                  conn; /* NULL for a bot (Net/Conn.h) */
    grenade_t*               grenade;
    uint64_t                 permissions;
    string_node_t*           current_periodic_message;
    timers_t                 timers;
    permissions_t            role_list[5]; // Change me based on the number of access levels you require
    state_t                  state;
    weapon_t                 weapon;
    team_t                   team;
    tool_t                   item;
    weapon_default_clip_t    default_clip;
    weapon_default_reserve_t default_reserve;
    uint32_t                 kills;
    uint32_t                 deaths;
    uint32_t                 assists;     /* Assists.c: kills it helped with */
    /* match stats (Server/Stats.c): reset when the map starts, shown by /stats
       and the end-of-match summary; score also carries survival points in
       the zombie mode and travels in PlayerUpdate.score */
    uint32_t                 score;
    uint32_t                 captures;
    uint32_t                 shots;
    uint32_t                 hits;
    uint32_t                 blocks_placed;
    damage_log_t             damage_log[DAMAGE_LOG_SIZE]; /* recent enemy damage taken (Assists.c) */
    uint8_t                  damage_next;
    int                      version_minor;
    int                      version_major;
    int                      version_revision;
    color_t                  tool_color;
    int                      hp;
    float                    lastclimb;
    ip_t                     ip;
    vector3f_t               locAtClick;
    movement_t               movement;
    vector3i_t               result_line[50];
    uint16_t                 ups;
    char                     client;
    uint8_t                  id;
    uint8_t                  respawn_time;
    uint8_t                  grenades;
    uint16_t                 blocks;        /* the block wallet (a class's start,
                                               game_class_info.blocks; the dig and the
                                               restock fill to game_class_info.blocks_max;
                                               a classless player keeps the classic 50) */
    uint8_t                  weapon_reserve;
    uint8_t                  weapon_clip;
    uint8_t                  weapon_pellets;
    uint8_t                  gun_hits;      /* block hits counted in the gun's last shot (Block.c: pellets, or a class's breaks) */
    uint8_t                  alive;
    uint8_t                  input;
    uint8_t                  allow_killing;
    uint8_t                  allow_team_killing;
    uint8_t                  muted;
    uint8_t                  admin_muted;
    uint8_t                  can_build;
    uint8_t                  has_intel;
    uint8_t                  squad;      /* 0 none, 1.. a squad of its team (Gamemodes/Squads.c) */
    uint8_t                  squad_team; /* the team squad belongs to: a team change drops it */
    uint8_t                  klass;         /* game_class_id (Server/Classes.c), Commando by default */
    float                    class_speed;   /* ground speed factor of the class, 1 without classes */
    float                    class_sprint;  /* its sprint, which REPLACES the walk speed on the ground
                                               (retail); 0 without classes: the classic x1.3 */
    float                    class_water;   /* its water friction figure; 0 without classes: the classic 6 */
    float                    jump_velocity; /* the class's jump, -0.36 without classes (Util/Physics.c) */
    /* the fall being flown down, in blocks (Util/Physics.c): a classed
       player's fall damage is its distance (the retail curve, fall_min',
       fall_max' and fall_dmg' of classes.h), not the landing speed; a move
       up clears it, landing applies it and zeroes it */
    float                    fall_dist;
    uint64_t                 rocket_at;     /* game clock of the last rocket fired (Proto10/Rockets.c) */
    uint8_t                  drill_charges; /* the Miner's drill (Server/Classes.c) */
    uint64_t                 drill_at;      /* game clock of the last drill */
    /* free flight (creative, Util/Physics.c with common/flight's
       flight_free): set by the mode every tick; fly_up is the held jump
       (the 1.1 thrust bit) */
    uint8_t                  flying, fly_up;
    /* the 1.0 input flags of the last update: a weapon fired by the trigger
       (the rocket, the drill: Proto10/Bridge.c) fires on its press */
    uint8_t                  p10_input_prev;
    /* the reference's analogue input: how far the stick is pushed, 0..1 of a
       walk (1 for keys and bots); the physics scales the ground's push */
    float                    move_scale;
    /* get_nanos() of the last change of the item in hand (SetTool.c): a gun
       fires GAME_WEAPON_RAISE_MS after it is taken out (protocol 1.0) */
    uint64_t                 item_at;
    /* the gun not in hand (a class's secondary): 255 none */
    uint8_t                  alt_weapon;
    uint8_t                  alt_clip, alt_reserve, alt_default_clip, alt_default_reserve;
    /* which of the class's guns is in hand: 0 the primary (1.0's Weapon1),
       1 the secondary (Weapon2); the tool slot carries it to the clients */
    uint8_t                  gun_slot;
    uint64_t                 swapped_at;
    uint64_t                 hurt_at;    /* get_nanos() of the last damage: a squadmate in a fight is no spawn */
    uint8_t                  is_invisible;
    uint8_t                  is_bot; // 1 when this player is driven by the server AI (Bots.c), has no peer
    uint8_t                  spawn_rect_override; // test hook: use team A's spawn rectangle
    uint8_t                  spawn_beside;        // id + 1 of the player it spawns next to (the tutorial's target), 0 none
    /* CTF's flank approach (Ctf.c): 0 not picked, 1 on the way to the staging
       point, 2 past it. Picked once per life for an attacker on a large map
       without a squad lane. */
    uint8_t                  flank_stage;
    float                    flank_x, flank_y;
    uint8_t                  welcome_sent;
    uint8_t                  reloading;
    /* flood limits (Server/Flood.c): a token bucket per kind of input */
    flood_bucket_t           flood[3];
    uint8_t                  periodic_delay_index;
    uint8_t                  invalid_pos_count;
    /* spawn protection (Server/SpawnProtect.c): enemy damage is refused until
       this game clock; firing clears it */
    uint64_t                 spawn_protected_until;
    /* moderation (Server/Conduct.c) */
    uint64_t                 afk_since;      /* last real input */
    uint8_t                  afk_last_flags; /* movement/fire/aim bits of that input */
    vector3f_t               afk_last_orientation;
    uint8_t                  afk_warned;
    float                    grief_points;
    uint64_t                 grief_decay_at;
    uint8_t                  grief_warned;
    /* anti-cheat counters (Server/Anticheat.c); an index per check kind */
#define ANTICHEAT_KINDS 8
    uint16_t                 anticheat_count[ANTICHEAT_KINDS];
    uint64_t                 anticheat_log_at[ANTICHEAT_KINDS];
    /* escape watch (Server/EscapeWatch.c) */
    uint64_t                 escape_since;
    uint8_t                  escape_kind;  /* EscapeWatchKind, 0 none */
    uint8_t                  escape_marked;
    /* lag compensation (Server/LagComp.c) */
    lag_sample_t             lag_history[LAG_HISTORY_SIZE];
    uint8_t                  lag_next;
    uint8_t                  lag_count;
    uint8_t                  move_forward;
    uint8_t                  move_backwards;
    uint8_t                  move_left;
    uint8_t                  move_right;
    uint8_t                  jumping;
    uint8_t                  crouching;
    uint8_t                  sneaking;
    uint8_t                  sprinting;
    uint8_t                  primary_fire;
    uint8_t                  secondary_fire;
    uint8_t                  airborne;
    uint8_t                  wade;
    uint8_t                  next_shot_invalid;
    uint8_t                  protocol;     // wire_protocol_t
    uint8_t                  p10_state;    // Proto10/Session.h, only for PROTOCOL_10
    /* jetpack perk (Server/Proto10/Jetpack.c, common/flight/flight.h); only a
       1.1 client sets the input, bots never do */
    uint8_t                  jetpack_equipped;      // granted for this life (jetpack_grant)
    uint8_t                  jetpack_input;         // the owner holds thrust (P10_IN_JETPACK)
    uint8_t                  jetpack_input_refused; // last input was refused (logged once per press)
    uint8_t                  light_on;              // protocol 1.0 flashlight (`light` GenericCommand), off on death/spawn/team/leave/map
    uint64_t                 light_at;              // get_nanos() of the last accepted light change (rate limit)
    uint8_t                  device_platform;       // `device` GenericCommand (common/devices/devices.h), 0 = unknown
    uint8_t                  device_input;          // the client's last input device, for the scoreboard
	uint8_t                  device_family;         // controller maker (xbox/playstation/nintendo), 0 = unknown
    uint8_t                  high_jump;             // the mode's rules: the zombies' jump (GM_RULE_HIGH_JUMP)
    uint8_t                  leap_charge;           // the next jump's leap (game_zombie_leap_param), 0 a normal jump
    uint8_t                  fast;                  // the mode's rules: the zombies' run (GM_RULE_FAST)
    uint8_t                  slowed;                // hit a moment ago: GAME_ZOMBIE_SLOWED (zombie_slow_until)
    uint64_t                 zombie_slow_until;     // game clock; set by the zombie mode on a hit
    uint64_t                 zombie_leap_ready;     // game clock; the high jump is back (GAME_ZOMBIE_LEAP_COOLDOWN)
    uint8_t                  jetpack_thrusting;     // thrust applied in the last physics step
    float                    jetpack_fuel;          // seconds of thrust left
    uint64_t                 p10_deadline; // nanos: the current 1.0 handshake step must end by then
    void*                    p10_map_job;     // the ngmap encoding it waits for (Session.c), or NULL
    /* what 1.0 clients were last told about this player (Proto10/Bridge.c);
       kept for every player, bots and 0.75 players included */
    uint8_t                  p10_announced;   // PlayerUpdate Create sent
    uint8_t                  p10_entity_live; // its Player entity exists on 1.0 clients
    uint8_t                  p10_entity_died; // an EntityDie went out since the last entity tick
    uint8_t                  p10_last_team;
    uint32_t                 p10_last_kills;
    uint32_t                 p10_last_deaths;
    uint32_t                 p10_last_score;
    uint32_t                 p10_last_ping;
    uint8_t                  p10_last_flags; // PlayerUpdate's Flags last sent (isAdmin)
    uint8_t                  typing;         // PlayerInput's chat bit: the chat box is open
    uint64_t                 p10_ping_sent_at;
    p10_entity_item          p10_last_entity;
    /* the seat the player holds (Server/Proto10/Mount.c): the entity's id,
       P10_NO_OWNER when the player is on nothing, and which seat of it; a
       helicopter's occupant cannot be shot (mount_in_vehicle) */
    uint32_t                 mount_entity;
    uint8_t                  mount_seat;
    uint8_t                  mount_vehicle; /* 1: a helicopter, 0: an MG */    uint8_t                  p10_map_quality; // ngmap quality (0 = vxl) from InitiateConnection
    uint32_t                 p10_map_crc32;   // CRC32 of the MapData stream: the client's cache key (Session.c)
    uint8_t                  p10_nonce[128];  // our Greeting's nonce: a connect token names it (Net/Master.c)
    /* the map transfer's progress, for its one map_sent line (Session.c) */
    size_t                   p10_map_sent_bytes; // payload bytes handed to the transport
    uint32_t                 p10_map_sent_ticks; // ticks that sent at least one fragment
    uint64_t                 p10_map_sent_at;    // get_real_nanos() when the transfer began
    uint32_t                 p10_host_status; // last "host-status" sent (Bridge.c), 0 = never
    uint32_t                 p10_host_bots;   // last "host-bots" sent, 0 = never
    /* perks (Server/Perks.c, docs/protocol-1.0.md 12.5); each perk has its own
       progress, reset on death unless [perks.<name>] reset_on_death = false */
    uint8_t                  perk_points[P10_PERK_COUNT]; // progress earned this life, per perk
    uint8_t                  perk_inventory; // 1 << P10_PERK_* for each perk held
    uint8_t                  perk_granted;   // perks already granted this life (one of each)
    uint8_t                  perk_sent;      // the owner has the values below
    uint8_t                  perk_sent_points;
    uint8_t                  perk_sent_inventory;
    uint8_t                  perk_sent_next;
    char                     name[PLAYER_NAME_STRLEN + 1];
    char                     os_info[255];
} player_t;


#endif
