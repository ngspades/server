// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Protocol 1.0 packet codec, shared by the client and the server.
 * See docs/protocol-1.0.md sections 5 to 10 and 12.2.
 *
 * Every packet is one ENet packet: a type byte, then the payload.
 *
 * - Fixed packets have a struct, p10_encode_<name>() and p10_decode_<name>().
 *   Decoders return 1 on success and 0 for a malformed packet (truncated,
 *   trailing bytes, out of range values). The server disconnects the peer
 *   with P10_DISCONNECT_MALFORMED_PACKET on 0.
 * - Packets that carry lists are decoded with an iterator, so nothing is
 *   allocated: p10_begin_<name>() checks the header and leaves the reader on
 *   the first item, then p10_next_<item>() returns 1 per item, 0 at the end,
 *   and the reader's error flag tells a clean end from a malformed one.
 *   Encoders mirror that: p10_start_<name>() then one p10_put_<item>() each.
 * - Decoded bytes/strings are views into the packet buffer: copy them before
 *   the buffer is released.
 *
 * Encoders never fail on their own; check w->error once at the end (it is set
 * when the buffer is too small).
 */
#ifndef PROTO10_CODEC_H
#define PROTO10_CODEC_H

#include "buf.h"

/* ENet connect data that selects protocol 1.0 (0.75 sends 3, 0.76 sends 4) */
#define P10_CONNECT_MAGIC 0x08101919u
#define P10_PROTOCOL_NAME "WorldOfSpades 1.0"
/* the protocol's revision, informational only (logs, the master's "1.2"):
   every 1.0 peer speaks the whole protocol, jetpack bits and compact
   trajectories included, so nothing is negotiated on it
   (docs/protocol-1.0.md 12.1) */
#define P10_VERSION_MAJOR 1
/* revision 1.3: Player Entities carry all three weapon slots filled
   (weapon[2] is the gear, below) */
#define P10_VERSION_MINOR 3
#define P10_GREETING_MAGIC "Hello"

typedef enum {
    P10_PKT_GREETING               = 1,
    P10_PKT_INITIATE_CONNECTION    = 2,
    P10_PKT_SERVER_CERTIFICATE     = 3,
    P10_PKT_CLIENT_CERTIFICATE     = 4,
    P10_PKT_KICK                   = 5,
    P10_PKT_GAME_STATE_HEADER      = 6,
    P10_PKT_MAP_DATA               = 7,
    P10_PKT_GAME_STATE_FINAL       = 8,
    P10_PKT_GENERIC_COMMAND        = 9,
    P10_PKT_MAP_DATA_ACKNOWLEDGE   = 10,
    P10_PKT_MAP_DATA_FINAL         = 11,
    P10_PKT_ENTITY_UPDATE          = 30,
    P10_PKT_ENTITY_EVENT           = 31,
    P10_PKT_ENTITY_DIE             = 32,
    P10_PKT_ENTITY_REMOVE          = 33,
    P10_PKT_CLIENT_ENTITY_UPDATE   = 35,
    P10_PKT_TERRAIN_UPDATE         = 40,
    P10_PKT_PLAYER_UPDATE          = 50,
    P10_PKT_PLAYER_REMOVE          = 51,
    P10_PKT_PLAYER_ACTION          = 60,
    P10_PKT_HIT_ENTITY             = 90,
    P10_PKT_HIT_TERRAIN            = 91,
    P10_PKT_DAMAGE                 = 92,
    /* [1.0] additions, section 12.2 */
    P10_PKT_CHAT_MESSAGE           = 70,
    P10_PKT_CHAT_BROADCAST         = 71,
    P10_PKT_GAME_MODE_STATE        = 80,
    P10_PKT_BUILD_BLOCK            = 93,
    P10_PKT_BUILD_LINE             = 94,
    P10_PKT_THROW_GRENADE          = 95,
} p10_packet_type;

/* ENet disconnect data on a 1.0 connection (vanilla uses another table) */
typedef enum {
    P10_DISCONNECT_UNKNOWN           = 0,
    P10_DISCONNECT_INTERNAL_ERROR    = 1,
    P10_DISCONNECT_SERVER_FULL       = 2,
    P10_DISCONNECT_MISC              = 3, /* a Kick with the text was sent first */
    P10_DISCONNECT_SERVER_STOPPED    = 4,
    P10_DISCONNECT_TIMEOUT           = 5,
    P10_DISCONNECT_MALFORMED_PACKET  = 6,
    P10_DISCONNECT_PROTOCOL_MISMATCH = 7,
} p10_disconnect_reason;

typedef enum {
    P10_ENT_PLAYER,
    P10_ENT_GRENADE,
    P10_ENT_ROCKET,
    P10_ENT_COMMAND_POST,
    P10_ENT_FLAG,
    P10_ENT_CHECKPOINT,
    P10_ENT_VEHICLE,
    /* [1.0] additions, docs/protocol-1.0.md 12.5. Their Create part carries
       an owner and a team (P10_ENT_HAS_OWNER) */
    P10_ENT_SENTRY,
    /* the classes' perks (12.6): placed like the sentry, owner and team too */
    P10_ENT_LANDMINE, /* the Marksman's: blows up when an enemy steps by it */
    P10_ENT_DYNAMITE, /* the Miner's: blows up after its fuse */
    P10_ENT_DRILL,    /* the Miner's drill bit: a Gravity trajectory in the air,
                         Linear while it bores (common/classes/classes.h) */
    /* supply crates (AoS 1.0 alpha's AMMO_CRATE and HEALTH_CRATE): no owner, neutral; a Linear trajectory while they
       fall, a Constant one on the ground */
    P10_ENT_AMMO_CRATE,
    P10_ENT_HEALTH_CRATE,
    /* the Commando's machine gun emplacement (common/mg/mg.h): owner and
       team; its rotation is the barrel's aim, its health the health part */
    P10_ENT_MG,
    /* a team's helicopter (common/vehicles/heli.h): no owner, the team;
       Linear trajectory with its velocity, rotation = yaw and tilts */
    P10_ENT_HELICOPTER,
    P10_ENT_COUNT
} p10_entity_type;

/* entity types whose Create part is followed by owner (varint) and team (u8) */
/* and the objectives (CommandPost, Flag, Checkpoint): no owner, a team
   (a Checkpoint, a place anyone takes: neutral) */
#define P10_ENT_HAS_OWNER(type)                                                                                   \
    ((type) >= P10_ENT_SENTRY || (type) == P10_ENT_COMMAND_POST || (type) == P10_ENT_FLAG ||                         \
     (type) == P10_ENT_CHECKPOINT)
/* owner of an entity no player owns */
#define P10_NO_OWNER 0xFFFFFFFFu

/* P10_EV_FIRE is a [1.0] addition (12.4): the entity fired along its
   trajectory's rotation; param bit 0 = the shot hit something.
   P10_EV_USE_PERK (12.5) is a PlayerAction only: param = p10_perk_param() */
typedef enum {
    P10_EV_JUMP,
    P10_EV_RELOAD_WEAPON,
    P10_EV_SUICIDE,
    P10_EV_EXPLODE,
    P10_EV_FIRE,
    P10_EV_USE_PERK
} p10_event_type;
typedef enum { P10_DEATH_UNSPECIFIED, P10_DEATH_PLAYER, P10_DEATH_EXPLODE } p10_death_type;
typedef enum { P10_TOOL_MELEE, P10_TOOL_BLOCK, P10_TOOL_WEAPON1, P10_TOOL_WEAPON2, P10_TOOL_WEAPON3 } p10_tool_slot;
typedef enum { P10_STANCE_STAND, P10_STANCE_CROUCH, P10_STANCE_PRONE } p10_stance;
/* P10_WEAPON_GEAR is ours, not ngspades': the 2-bit field's free value. It is
   what weapon[2] (Weapon3, ToolSlot 4) holds: the gear (grenades, the Miner's
   drill), its magazine the count and its fire_interval_ms the rate */
typedef enum { P10_WEAPON_BULLET, P10_WEAPON_SHOTGUN, P10_WEAPON_ROCKET, P10_WEAPON_GEAR } p10_weapon_type;
typedef enum { P10_HIT_UNSPECIFIED, P10_HIT_TORSO, P10_HIT_HEAD, P10_HIT_LIMB } p10_hit_type;
typedef enum {
    P10_DMG_UNKNOWN,
    P10_DMG_SUICIDE,
    P10_DMG_FALL,
    P10_DMG_POISON,
    P10_DMG_MELEE,
    P10_DMG_DIG,
    P10_DMG_BLOCK,
    P10_DMG_WEAPON1,
    P10_DMG_WEAPON2,
    P10_DMG_WEAPON3,
    P10_DMG_GRENADE,
    P10_DMG_VEHICLE_EXPLOSION,
    P10_DMG_VEHICLE,
    P10_DMG_SENTRY /* [1.0] (12.4): shot by a sentry gun */
} p10_damage_type;
typedef enum { P10_BLOCK_CREATE_UNSPECIFIED, P10_BLOCK_CREATE_PLAYER } p10_block_create;
typedef enum {
    P10_BLOCK_DESTROY_UNSPECIFIED,
    P10_BLOCK_DESTROY_DAMAGE,
    P10_BLOCK_DESTROY_COLLAPSE /* the block fell because its support was removed */
} p10_block_destroy;

/* 1 when the packet is non-empty; *type gets the first byte */
int p10_packet_type_of(const uint8_t* data, size_t size, uint8_t* type);

/* ---- session (section 6) ---- */

typedef struct {
    p10_bytes nonce;
} p10_greeting;

typedef struct {
    p10_bytes protocol_name;
    uint16_t major, minor, revision;
    p10_bytes package;
    p10_bytes environment;
    p10_bytes locale;
    p10_bytes player_name;
    p10_bytes nonce;
    uint8_t map_quality;
} p10_initiate_connection;

/* used for both ServerCertificate (3) and ClientCertificate (4) */
typedef struct {
    uint8_t is_valid;
    p10_bytes certificate;
    p10_bytes signature;
} p10_certificate;

typedef struct {
    p10_bytes reason;
} p10_kick;

void p10_encode_greeting(p10_writer* w, const p10_greeting* p);
int p10_decode_greeting(const uint8_t* data, size_t size, p10_greeting* p);
void p10_encode_initiate_connection(p10_writer* w, const p10_initiate_connection* p);
int p10_decode_initiate_connection(const uint8_t* data, size_t size, p10_initiate_connection* p);
/* type is P10_PKT_SERVER_CERTIFICATE or P10_PKT_CLIENT_CERTIFICATE */
void p10_encode_certificate(p10_writer* w, uint8_t type, const p10_certificate* p);
int p10_decode_certificate(const uint8_t* data, size_t size, uint8_t type, p10_certificate* p);
void p10_encode_kick(p10_writer* w, const p10_kick* p);
int p10_decode_kick(const uint8_t* data, size_t size, p10_kick* p);

/* ---- empty packets: MapDataAcknowledge (10), MapDataFinal (11) ---- */

void p10_encode_empty(p10_writer* w, uint8_t type);
int p10_decode_empty(const uint8_t* data, size_t size, uint8_t type);

/* ---- string maps (GameStateHeader, GameStateFinal properties) ---- */

/* writes one key/value pair; empty keys are skipped like the reference does */
void p10_put_map_entry(p10_writer* w, const char* key, const char* value);
void p10_put_map_entry_bytes(p10_writer* w, p10_bytes key, p10_bytes value);
void p10_put_map_end(p10_writer* w);
/* 1 per pair; 0 at the terminating empty key (or on error) */
int p10_next_map_entry(p10_reader* r, p10_bytes* key, p10_bytes* value);

/* ---- game state transfer (section 7) ---- */

/* GameStateHeader (6): p10_start_game_state_header, entries, p10_put_map_end */
void p10_start_game_state_header(p10_writer* w);
int p10_begin_game_state_header(p10_reader* r, const uint8_t* data, size_t size);

typedef struct {
    p10_bytes fragment;
} p10_map_data;

void p10_encode_map_data(p10_writer* w, const p10_map_data* p);
int p10_decode_map_data(const uint8_t* data, size_t size, p10_map_data* p);

/* ---- entities (section 8) ---- */

enum {
    P10_EU_CREATE       = 1 << 0,
    P10_EU_FLAGS        = 1 << 1,
    P10_EU_TRAJECTORY   = 1 << 2,
    P10_EU_PLAYER_INPUT = 1 << 3,
    P10_EU_TOOL         = 1 << 4,
    P10_EU_BLOCK_COLOR  = 1 << 5,
    P10_EU_HEALTH       = 1 << 6,
    P10_EU_KNOWN        = 0x7f,
};

enum {
    P10_EF_PLAYER_CLIP = 1 << 0,
    P10_EF_WEAPON_CLIP = 1 << 1,
    P10_EF_FLY         = 1 << 2,
    P10_EF_JETPACK     = 1 << 3, /* revision 1.1: this body wears an equipped jetpack */
};

typedef enum {
    P10_TRAJ_CONSTANT,
    P10_TRAJ_LINEAR,
    P10_TRAJ_GRAVITY,
    P10_TRAJ_PLAYER,
    P10_TRAJ_RIGID_BODY,
    /* revision 1.2: a Player trajectory in 17 bytes instead of 37 (fixed
       point, docs/protocol-1.0.md 12.7). Only ever on the wire: the encoder
       writes it for an item asking for it when every value fits (else the
       float Player form), the decoder returns it as P10_TRAJ_PLAYER */
    P10_TRAJ_PLAYER_COMPACT,
    P10_TRAJ_COUNT
} p10_trajectory_type;

typedef struct {
    uint8_t type; /* p10_trajectory_type */
    p10_vec3 origin;
    p10_vec3 velocity;
    p10_vec3 rotation;         /* compact quaternion, all types but Player */
    p10_vec3 angular_velocity; /* all types but Player */
    p10_vec3 euler;            /* Player only */
    uint8_t has_parent;
    uint32_t parent;
} p10_trajectory;

enum {
    P10_IN_TOOL_PRIMARY   = 1 << 0,
    P10_IN_TOOL_SECONDARY = 1 << 1,
    P10_IN_CHAT           = 1 << 2,
    P10_IN_SPRINT         = 1 << 3,
    P10_IN_JETPACK        = 1 << 4, /* revision 1.1: wants jetpack thrust */
};

typedef struct {
    uint8_t flags;  /* P10_IN_* */
    uint8_t stance; /* p10_stance */
    int8_t xmove;   /* strafe, -127..127 */
    int8_t ymove;   /* forward, -127..127 */
} p10_player_input;

typedef struct {
    uint8_t full_auto;
    uint8_t slow_reload;
    uint8_t penetrative;
    uint8_t type; /* p10_weapon_type */
    uint16_t fire_interval_ms;
    uint16_t reload_ms;
    uint16_t raise_ms;
    uint8_t max_damage; /* percent */
    uint8_t magazine;
} p10_weapon_params;

typedef struct {
    uint32_t entity_id;
    uint8_t mask; /* P10_EU_* */
    /* P10_EU_CREATE */
    uint8_t entity_type;          /* p10_entity_type */
    p10_bytes body_skin;          /* Player only */
    p10_bytes weapon_skin[3];     /* Player only */
    p10_weapon_params weapon[3];  /* Player only */
    uint32_t owner;               /* P10_ENT_HAS_OWNER types: player id or P10_NO_OWNER */
    uint8_t team;                 /* P10_ENT_HAS_OWNER types: P10_TEAM_1, P10_TEAM_2 or P10_TEAM_SPECTATOR */
    /* the other parts, each present when its mask bit is set */
    uint8_t flags; /* P10_EF_* */
    p10_trajectory trajectory;
    p10_player_input input;
    uint8_t tool; /* p10_tool_slot */
    p10_color block_color;
    uint8_t health;
} p10_entity_item;

void p10_put_entity_item(p10_writer* w, const p10_entity_item* item);
int p10_next_entity_item(p10_reader* r, p10_entity_item* item);

/* EntityUpdate (30): forced = apply even to entities the client owns */
void p10_start_entity_update(p10_writer* w, int forced);
int p10_begin_entity_update(p10_reader* r, const uint8_t* data, size_t size, int* forced);

/* ClientSideEntityUpdate (35): items never carry P10_EU_CREATE */
void p10_start_client_entity_update(p10_writer* w);
int p10_begin_client_entity_update(p10_reader* r, const uint8_t* data, size_t size);

/* ---- players ---- */

enum {
    P10_PU_CREATE = 1 << 0,
    P10_PU_SCORE  = 1 << 1,
    P10_PU_FLAGS  = 1 << 2,
    /* [1.0] additions, written after Score in this order */
    P10_PU_TEAM  = 1 << 3,
    P10_PU_STATS = 1 << 4, /* kills, deaths */
    P10_PU_PING  = 1 << 5,
    P10_PU_PERKS = 1 << 6, /* streak points, perk inventory, next threshold (12.5) */
    P10_PU_KNOWN = 0x7f,
};

/* perks (docs/protocol-1.0.md 12.5); the inventory is a bitmask of 1 << id */
#define P10_PERK_JETPACK 0
#define P10_PERK_SENTRY  1
#define P10_PERK_REVIVE  2 /* being removed: the Commando's perk is the MG now (P10_PERK_MG) */
#define P10_PERK_MG      2 /* the Commando's streak perk: a machine gun placed like the sentry */
#define P10_PERK_LANDMINE 3 /* the Marksman's streak perk, placed like the sentry (12.6) */
#define P10_PERK_DYNAMITE 4 /* the Miner's streak perk, placed like the sentry (12.6) */
#define P10_PERK_COUNT   5

/* PlayerAction P10_EV_USE_PERK param: bits 0-7 perk id, and for a perk placed
   in the world (the sentry) the block cell it stands on: bits 8-19 x,
   bits 20-31 y, bits 32-39 z (the solid floor block, map coordinates) */
uint64_t p10_perk_param(uint8_t perk, uint32_t x, uint32_t y, uint32_t z);
/* 0 when bits above 39 are set (nothing else to check: every field fits) */
int p10_perk_param_decode(uint64_t param, uint8_t* perk, uint32_t* x, uint32_t* y, uint32_t* z);

#define P10_TEAM_1         0
#define P10_TEAM_2         1
#define P10_TEAM_SPECTATOR 255

enum { P10_PF_ADMIN = 1 << 0 };

typedef struct {
    uint32_t player_id;
    uint8_t mask;   /* P10_PU_* */
    p10_bytes name; /* P10_PU_CREATE */
    uint8_t flags;  /* P10_PU_FLAGS, P10_PF_* */
    uint32_t score; /* P10_PU_SCORE */
    uint8_t team;   /* P10_PU_TEAM: P10_TEAM_1, P10_TEAM_2 or P10_TEAM_SPECTATOR */
    uint32_t kills; /* P10_PU_STATS */
    uint32_t deaths;
    uint32_t ping_ms; /* P10_PU_PING */
    uint8_t perk_points;    /* P10_PU_PERKS: streak points of this life */
    uint8_t perk_inventory; /* P10_PU_PERKS: 1 << P10_PERK_* for each perk held */
    uint8_t perk_next;      /* P10_PU_PERKS: points of the next perk, 0 = none left */
} p10_player_item;

/* wire order is Create, Flags, Score (not the bit order), then Team, Stats, Ping, Perks */
void p10_put_player_item(p10_writer* w, const p10_player_item* item);
int p10_next_player_item(p10_reader* r, p10_player_item* item);

void p10_start_player_update(p10_writer* w);
int p10_begin_player_update(p10_reader* r, const uint8_t* data, size_t size);

/* PlayerRemove (51): a list of player ids */
void p10_start_player_remove(p10_writer* w);
void p10_put_player_id(p10_writer* w, uint32_t player_id);
int p10_begin_player_remove(p10_reader* r, const uint8_t* data, size_t size);
int p10_next_player_id(p10_reader* r, uint32_t* player_id);

/* GameStateFinal (8):
 *   p10_start_game_state_final, properties, p10_put_map_end,
 *   p10_put_count(entities), entity items,
 *   p10_put_count(players), player items.
 * Decode: p10_begin_game_state_final, p10_next_map_entry until 0,
 *   p10_read_count, that many p10_next_entity_item, p10_read_count,
 *   that many p10_next_player_item, then p10_at_end. */
void p10_start_game_state_final(p10_writer* w);
int p10_begin_game_state_final(p10_reader* r, const uint8_t* data, size_t size);
void p10_put_count(p10_writer* w, uint32_t count);
uint32_t p10_read_count(p10_reader* r);

/* ---- GenericCommand (9): a list of strings ---- */

void p10_start_generic_command(p10_writer* w);
void p10_put_generic_part(p10_writer* w, const char* part);
int p10_begin_generic_command(p10_reader* r, const uint8_t* data, size_t size);
int p10_next_generic_part(p10_reader* r, p10_bytes* part);

/* ---- events ---- */

typedef struct {
    uint32_t entity_id;
    uint8_t type; /* p10_event_type */
    uint64_t param;
} p10_entity_event;

typedef struct {
    uint8_t has_from_entity;
    uint32_t from_entity;
    uint8_t damage_type; /* p10_damage_type */
    p10_vec3 fire_position;
    p10_vec3 hit_position;
} p10_damage_info;

typedef struct {
    uint32_t entity_id;
    uint8_t type; /* p10_death_type */
    uint64_t param;
    uint8_t has_damage;
    p10_damage_info damage;
} p10_entity_die;

typedef struct {
    uint32_t entity_id;
} p10_entity_remove;

typedef struct {
    uint8_t type; /* p10_event_type */
    uint64_t param;
} p10_player_action;

void p10_encode_entity_event(p10_writer* w, const p10_entity_event* p);
int p10_decode_entity_event(const uint8_t* data, size_t size, p10_entity_event* p);
/* fixes D3: a death without damage info decodes */
void p10_encode_entity_die(p10_writer* w, const p10_entity_die* p);
int p10_decode_entity_die(const uint8_t* data, size_t size, p10_entity_die* p);
/* fixes D5 by construction: decodes into its own type */
void p10_encode_entity_remove(p10_writer* w, const p10_entity_remove* p);
int p10_decode_entity_remove(const uint8_t* data, size_t size, p10_entity_remove* p);
void p10_encode_player_action(p10_writer* w, const p10_player_action* p);
int p10_decode_player_action(const uint8_t* data, size_t size, p10_player_action* p);

/* ---- terrain (section 9.1) ---- */

typedef struct {
    int32_t x, y, z;
    uint8_t health;  /* 0 = destroyed */
    p10_color color; /* only when health != 0 */
    uint8_t cause;   /* p10_block_create when created, p10_block_destroy when destroyed */
} p10_map_edit;

typedef struct {
    int32_t cx, cy, cz;
} p10_terrain_cursor;

void p10_start_terrain_update(p10_writer* w, p10_terrain_cursor* cursor);
/* coordinates must be 0..INT32_MAX (the cursor is an unsigned varint) */
void p10_put_map_edit(p10_writer* w, p10_terrain_cursor* cursor, const p10_map_edit* edit);
int p10_begin_terrain_update(p10_reader* r, const uint8_t* data, size_t size, p10_terrain_cursor* cursor);
int p10_next_map_edit(p10_reader* r, p10_terrain_cursor* cursor, p10_map_edit* edit);

/* ---- hits and damage (sections 9.2 to 9.4) ---- */

typedef struct {
    uint32_t entity_id;
    uint8_t hit_type;    /* p10_hit_type */
    uint8_t damage_type; /* p10_damage_type */
    p10_vec3 fire_position;
    p10_vec3 hit_position;
} p10_hit_entity;

typedef struct {
    int32_t x, y, z;
    uint8_t damage_type;
    p10_vec3 fire_position;
    p10_vec3 hit_position;
} p10_hit_terrain;

typedef struct {
    uint32_t entity_id;
    p10_damage_info damage;
    uint32_t to_entity;
    uint8_t amount;
} p10_damage;

void p10_encode_hit_entity(p10_writer* w, const p10_hit_entity* p);
int p10_decode_hit_entity(const uint8_t* data, size_t size, p10_hit_entity* p);
void p10_encode_hit_terrain(p10_writer* w, const p10_hit_terrain* p);
int p10_decode_hit_terrain(const uint8_t* data, size_t size, p10_hit_terrain* p);
/* fixes D4: fromEntity is written when its flag is set */
void p10_encode_damage(p10_writer* w, const p10_damage* p);
int p10_decode_damage(const uint8_t* data, size_t size, p10_damage* p);

/* ---- chat (section 12.2) ---- */

enum { P10_CHAT_ALL = 0, P10_CHAT_TEAM = 1, P10_CHAT_SYSTEM = 2 };

/* ChatBroadcast fromPlayer for messages that come from the server itself */
#define P10_CHAT_FROM_SERVER 0xFFFFFFFFu
/* longest chat text in bytes (UTF-8), the 0.75 limit; the server shortens
   longer 0.75 messages (CP437 grows when converted) before sending them */
#define P10_CHAT_MAX_TEXT 255u

typedef struct {
    uint8_t channel; /* P10_CHAT_ALL or P10_CHAT_TEAM */
    p10_bytes text;  /* UTF-8, at most P10_CHAT_MAX_TEXT bytes, no NUL */
} p10_chat_message;

typedef struct {
    uint32_t from_player; /* player id or P10_CHAT_FROM_SERVER */
    uint8_t channel;      /* P10_CHAT_ALL, P10_CHAT_TEAM or P10_CHAT_SYSTEM */
    p10_bytes text;
} p10_chat_broadcast;

/* The encoders set w->error instead of writing a message the decoder would
   reject (bad channel, text too long, invalid UTF-8 or NUL), so a bug shows
   up at the sender and not as a disconnect at the receiver. */
void p10_encode_chat_message(p10_writer* w, const p10_chat_message* p);
int p10_decode_chat_message(const uint8_t* data, size_t size, p10_chat_message* p);
void p10_encode_chat_broadcast(p10_writer* w, const p10_chat_broadcast* p);
int p10_decode_chat_broadcast(const uint8_t* data, size_t size, p10_chat_broadcast* p);

/* ---- GameModeState (80): a string map, section 12.2 ---- */

/* keys of the vocabulary; values are described in docs/protocol-1.0.md 12.2.
   "territory<N>" is formatted with P10_GM_TERRITORY_PREFIX and the index. */
#define P10_GM_MODE             "mode"
#define P10_GM_TEAM1_NAME       "team1-name"
#define P10_GM_TEAM2_NAME       "team2-name"
#define P10_GM_TEAM1_COLOR      "team1-color"
#define P10_GM_TEAM2_COLOR      "team2-color"
#define P10_GM_TEAM1_SCORE      "team1-score"
#define P10_GM_TEAM2_SCORE      "team2-score"
#define P10_GM_CAPTURE_LIMIT    "capture-limit"
#define P10_GM_TEAM1_INTEL      "team1-intel"
#define P10_GM_TEAM2_INTEL      "team2-intel"
/* The objectives' entities (12.2.3) have fixed ids: P10_OBJECTIVE_ID_BASE +
   0 team 1's intel, 1 team 2's, 2 team 1's tent, 3 team 2's, 4 KotH's hill,
   P10_OBJECTIVE_TERRITORY0 + i territory i (a Checkpoint). Far from the
   players (0..255), the grenades and the objects (256 up). */
#define P10_OBJECTIVE_ID_BASE    0x7FFF0000u
#define P10_OBJECTIVE_TERRITORY0 5
#define P10_TERRITORIES_MAX      8
#define P10_GM_TERRITORY_COUNT  "territory-count"
#define P10_GM_TERRITORY_PREFIX "territory"
#define P10_GM_PROGRESS         "progress"
/* arena (mode "arena"): waiting|countdown|fight|end, the round number, the
   seconds left in the countdown or the fight, and the last round's winner
   (team1, team2 or draw; empty before the first round ends) */
#define P10_GM_ARENA_STATE      "arena-state"
#define P10_GM_ARENA_ROUND      "arena-round"
#define P10_GM_ARENA_TIME       "arena-time"
#define P10_GM_ARENA_WINNER     "arena-winner"
/* zombie (mode "zombie"): its rounds use the arena-* keys above (state,
   round, seconds left, the last round's winner), plus the zombies' team
   (1 or 2) and the humans alive */
#define P10_GM_ZOMBIE_TEAM      "zombie-team"
#define P10_GM_ZOMBIE_HUMANS    "zombie-humans"
/* king of the hill (mode "koth"): the hill (x,y,z of its middle's ground,
   radius), fight|break (the pause after a round, the hill already moved),
   the round from 1, who holds it now (team1, team2, contested, none), the
   players of each team on it, this round's points of each team and the
   points that win it, and the last round's winner. The team scores count
   rounds won, capture-limit the rounds that win the map. */
#define P10_GM_KOTH_RADIUS      "koth-radius" /* the hill is a Checkpoint entity */
#define P10_GM_KOTH_FLOOR       "koth-floor"  /* map z of the cylinder's floor */
#define P10_GM_KOTH_STATE       "koth-state"
#define P10_GM_KOTH_ROUND       "koth-round"
#define P10_GM_KOTH_HOLDER      "koth-holder"
#define P10_GM_KOTH_ON          "koth-on"
#define P10_GM_KOTH_POINTS      "koth-points"
#define P10_GM_KOTH_WINNER      "koth-winner"
/* Plant the Bomb (mode "ptb"): its rounds use the arena-* keys above
   (arena-winner team1 = the counter-terrorists), plus how the last round
   ended (eliminated|bomb|defuse|time|draw, in the end state only), the
   bomb (none|carried|dropped|planted|defused|exploded), its carrier's id
   while carried, where it lies (x,y,z of its floor block) when dropped
   (never planted: no key says where the planted bomb is, nor its site), the
   fuse's whole seconds left and its length, who is defusing it, who is
   planting it (-1 nobody: the
   clients play the holds' sounds by these two), and each site (x,y,z,radius) */
#define P10_GM_PTB_WHY          "ptb-why"
#define P10_GM_PTB_BOMB         "ptb-bomb"
#define P10_GM_PTB_CARRIER      "ptb-carrier"
#define P10_GM_PTB_BOMB_AT      "ptb-bomb-at"
#define P10_GM_PTB_BOMB_TIME    "ptb-bomb-time"
#define P10_GM_PTB_BOMB_TOTAL   "ptb-bomb-total"
#define P10_GM_PTB_DEFUSER      "ptb-defuser"
#define P10_GM_PTB_PLANTER      "ptb-planter" /* who is planting it, -1 */
#define P10_GM_PTB_SITE_A       "ptb-site-a"
#define P10_GM_PTB_SITE_B       "ptb-site-b"

/* team field of territory<N> and progress: same numbers as the player team */
#define P10_GM_TEAM_NEUTRAL 255

/* p10_start_game_mode_state, p10_put_map_entry..., p10_put_map_end;
   decode with p10_begin_game_mode_state and p10_next_map_entry */
void p10_start_game_mode_state(p10_writer* w);
int p10_begin_game_mode_state(p10_reader* r, const uint8_t* data, size_t size);

/* Parses exactly count comma separated decimal integers ("12,-3,40"): an
   optional '-', digits, no spaces, each within int32. 1 on success. Every
   coordinate, colour and progress value of the vocabulary uses this form. */
int p10_parse_ints(p10_bytes s, int32_t* out, int count);

typedef enum { P10_INTEL_BASE, P10_INTEL_DROPPED, P10_INTEL_CARRIED } p10_intel_state;

/* An intel's state in GameModeState: "base", "dropped" or "carried". Where
   it is and who carries it are its Flag entity's (the trajectory's origin,
   and parentEntityId while carried: ngspades' design, 12.2.3); x, y, z and
   carrier are filled from that entity by the reader, never by the wire */
typedef struct {
    uint8_t state;     /* p10_intel_state */
    int32_t x, y, z;   /* BASE and DROPPED: the block cell it rests on */
    uint32_t carrier;  /* CARRIED: player id */
} p10_intel;

/* "base", "dropped" or "carried"; 1 on success (only state is set) */
int p10_parse_intel(p10_bytes s, p10_intel* out);
/* the inverse, NUL terminated; 0 when out is too small (out is then "") */
int p10_format_intel(const p10_intel* in, char* out, size_t out_size);

/* ---- building and grenades (section 12.2) ---- */

typedef struct {
    int32_t x, y, z;
} p10_blockcoord;

typedef struct {
    p10_blockcoord pos;
    p10_color color;
} p10_build_block;

typedef struct {
    p10_blockcoord from, to;
    p10_color color;
} p10_build_line;

typedef struct {
    p10_vec3 origin;
    p10_vec3 velocity;
    float fuse; /* seconds until it explodes */
} p10_throw_grenade;

void p10_encode_build_block(p10_writer* w, const p10_build_block* p);
int p10_decode_build_block(const uint8_t* data, size_t size, p10_build_block* p);
void p10_encode_build_line(p10_writer* w, const p10_build_line* p);
int p10_decode_build_line(const uint8_t* data, size_t size, p10_build_line* p);
void p10_encode_throw_grenade(p10_writer* w, const p10_throw_grenade* p);
/* rejects non-finite values and a negative fuse: every float here feeds the
   server's physics straight from a client */
int p10_decode_throw_grenade(const uint8_t* data, size_t size, p10_throw_grenade* p);

#endif
