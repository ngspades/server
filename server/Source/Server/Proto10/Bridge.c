// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Flood.h>
#include <Server/GameApi.h>
#include <Server/MapVote.h>
#include <Server/Outfit.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Bots.h>
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/HostPackets.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/NetStats.h>
#include <Server/Proto10/Entities.h>
#include <Server/Perks.h>
#include <Server/Plugins.h>
#include <Server/Proto10/GameMode.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Helicopter.h>
#include <Server/Proto10/Mg.h>
#include <Server/Proto10/Sentry.h>
#include <Server/Proto10/Explosives.h>
#include <Server/Packets/Packets.h>
#include <Server/Staff.h>
#include <Server/Proto10/Rockets.h>
#include <Server/Proto10/Flags.h>
#include <Server/Proto10/Crates.h>
#include <Server/Proto10/Drills.h>
#include <Server/Proto10/Session.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Enums.h>
#include <Util/Weapon.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <Server/LagComp.h>
#include <math.h>
#include <uthash/uthash.h>
#include <enet/enet.h>
#include <classes/classes.h>
#include <devices/devices.h>
#include <pings/pings.h>
#include <proto10/orient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* big enough for every player of a full server in one packet */
#define BRIDGE_BUF_SIZE 65536

/* ping is resent at most this often, it changes every tick otherwise */
#define PING_RESEND_NANOS (2ull * NANO_IN_SECOND)

static uint8_t bridge_buf[BRIDGE_BUF_SIZE];

/* ---- mapping between the server state and 1.0 items ---- */

static uint8_t _tool_to_p10(const player_t* p)
{
    switch (p->item) {
        case TOOL_SPADE:
            return P10_TOOL_MELEE;
        case TOOL_BLOCK:
            return P10_TOOL_BLOCK;
        case TOOL_GRENADE:
            return P10_TOOL_WEAPON3;
        case TOOL_GUN:
        default:
            /* the tool slot carries which of the class's guns is in hand */
            return p->gun_slot ? P10_TOOL_WEAPON2 : P10_TOOL_WEAPON1;
    }
}

/* gun: which of the class's guns the slot names (0 primary, 1 secondary),
   -1 when the tool is not a gun */
static int _tool_from_p10(uint8_t tool, uint8_t* out, int* gun)
{
    *gun = -1;
    switch (tool) {
        case P10_TOOL_MELEE:
            *out = TOOL_SPADE;
            return 1;
        case P10_TOOL_BLOCK:
            *out = TOOL_BLOCK;
            return 1;
        case P10_TOOL_WEAPON1:
            *out = TOOL_GUN;
            *gun = 0;
            return 1;
        case P10_TOOL_WEAPON2:
            *out = TOOL_GUN;
            *gun = 1;
            return 1;
        case P10_TOOL_WEAPON3:
            *out = TOOL_GRENADE;
            return 1;
        default:
            return 0;
    }
}

/* the 0.75 classes as 1.0 weapons; damage is the body shot percentage */
static p10_weapon_params _weapon_params(player_t* owner, uint8_t weapon)
{
    p10_weapon_params p;
    memset(&p, 0, sizeof p);
    p.type     = P10_WEAPON_BULLET;
    p.raise_ms = GAME_WEAPON_RAISE_MS;
    switch (weapon) {
        case WEAPON_SMG:
            p.full_auto        = 1;
            p.fire_interval_ms = 100;
            p.reload_ms        = 2500;
            p.max_damage       = 29;
            p.magazine         = 30;
            break;
        case WEAPON_SHOTGUN:
            p.slow_reload      = 1;
            p.type             = P10_WEAPON_SHOTGUN;
            p.fire_interval_ms = 1000;
            p.reload_ms        = 500;
            p.max_damage       = 27;
            p.magazine         = 6;
            break;
        case GAME_WEAPON_REVOLVER:
            p.fire_interval_ms = GAME_REVOLVER_DELAY_MS;
            p.reload_ms        = GAME_REVOLVER_RELOAD_MS;
            p.max_damage       = GAME_REVOLVER_TORSO;
            p.magazine         = GAME_REVOLVER_CLIP;
            break;
        case GAME_WEAPON_ROCKET:
            /* the Commando's (Proto10/Rockets.c): fired with the "rocket"
               command, never a HitEntity */
            p.type             = P10_WEAPON_ROCKET;
            p.fire_interval_ms = (uint16_t) (ROCKET_INTERVAL * 1000.0f);
            p.reload_ms        = ROCKET_RELOAD_MS;
            p.max_damage       = 100;
            p.magazine         = ROCKET_CLIP;
            break;
        case WEAPON_RIFLE:
        default:
            p.fire_interval_ms = 500;
            p.reload_ms        = 2500;
            p.max_damage       = 49;
            p.magazine         = 10;
            if (weapon_class_sniper(owner)) {
                p.fire_interval_ms = GAME_SNIPER_DELAY_MS;
                p.reload_ms        = GAME_SNIPER_RELOAD_MS;
                p.max_damage       = GAME_SNIPER_BODY;
                p.magazine         = GAME_SNIPER_CLIP;
            }
            break;
    }
    return p;
}

static int _announced(player_t* p)
{
    return is_past_join_screen(p);
}

static int _entity_live(player_t* p)
{
    return _announced(p) && p->state == STATE_READY && p->alive && p->team != TEAM_SPECTATOR;
}

/* a weapon's skin is its name under the game's resources; static strings:
   the views stored between ticks (bridge_views[].last) keep the pointers */
static const char* _weapon_skin(uint8_t weapon)
{
    return weapon < GAME_WEAPONS ? game_class_weapons[weapon] : "";
}

/* "class/<name>" composed once: the class table's names never change, so a
   static buffer per class keeps a stable pointer */
static const char* _class_skin_name(uint8_t klass)
{
    static char names[GAME_CLASSES_COUNT][32];
    static int  done = 0;
    if (!done) {
        for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
            snprintf(names[k], sizeof names[0], "class/%s", game_class_default(k)->name);
        }
        done = 1;
    }
    return klass < GAME_CLASSES_COUNT ? names[klass] : "default";
}

static void _full_entity(player_t* p, p10_entity_item* it)
{
    memset(it, 0, sizeof *it);
    it->entity_id   = p->id;
    it->mask        = P10_EU_KNOWN;
    it->entity_type = P10_ENT_PLAYER;
    /* the class's two guns and the gear: which is which follows the gun in
       hand (Weapon1 primary, Weapon2 the put-away secondary) */
    uint8_t primary   = p->gun_slot ? p->alt_weapon : p->weapon;
    uint8_t secondary = p->gun_slot ? p->weapon : p->alt_weapon;
    it->weapon[0]     = _weapon_params(p, primary);
    if (secondary != 255) {
        it->weapon[1] = _weapon_params(p, secondary);
    }
    server_t*                     server = get_server();
    const struct game_class_info* c      = classes_of(server, p);
    it->weapon[2].type                   = P10_WEAPON_GEAR;
    it->weapon[2].fire_interval_ms       = 1000;
    it->weapon[2].magazine =
    (c != NULL && c->gear == GAME_GEAR_DRILL) ? (uint8_t) c->gear_count : p->grenades;
    it->flags = P10_EF_PLAYER_CLIP | P10_EF_WEAPON_CLIP;
    /* the reference's fly flag: this body flies free (creative), whatever
       the mode, so a client predicts it from the body and not the mode */
    if (p->flying) {
        it->flags |= P10_EF_FLY;
    }

    /* skins: a weapon's its name, the body the class's folder; they must
       outlive the encoding, so only static strings go in */
    it->body_skin       = p10_bytes_from_cstring(c != NULL ? _class_skin_name(p->klass) : "default");
    it->weapon_skin[0]  = p10_bytes_from_cstring(_weapon_skin(primary));
    it->weapon_skin[1]  = p10_bytes_from_cstring(secondary != 255 ? _weapon_skin(secondary) : "");
    it->weapon_skin[2]  = p10_bytes_from_cstring(c != NULL && c->gear == GAME_GEAR_DRILL ? "drill" : "grenade");

    it->trajectory.type     = P10_TRAJ_PLAYER;
    it->trajectory.origin.x = p->movement.position.x;
    it->trajectory.origin.y = p->movement.position.y;
    it->trajectory.origin.z = p->movement.position.z;
    it->trajectory.velocity.x = p->movement.velocity.x;
    it->trajectory.velocity.y = p->movement.velocity.y;
    it->trajectory.velocity.z = p->movement.velocity.z;
    it->trajectory.euler      = p10_euler_from_front(
    p->movement.forward_orientation.x, p->movement.forward_orientation.y, p->movement.forward_orientation.z);

    /* the push as it came (analogue), so the others' clients walk it so */
    int push         = p->move_scale > 0.0f && p->move_scale < 1.0f ? (int) lroundf(127.0f * p->move_scale) : 127;
    it->input.xmove  = (int8_t) (push * ((int) p->move_right - (int) p->move_left));
    it->input.ymove  = (int8_t) (push * ((int) p->move_forward - (int) p->move_backwards));
    it->input.stance = p->crouching ? P10_STANCE_CROUCH : P10_STANCE_STAND;
    it->input.flags  = (uint8_t) ((p->primary_fire ? P10_IN_TOOL_PRIMARY : 0) |
                                 (p->secondary_fire ? P10_IN_TOOL_SECONDARY : 0) | (p->sprinting ? P10_IN_SPRINT : 0) |
                                 (p->typing ? P10_IN_CHAT : 0));

    it->tool          = _tool_to_p10(p);
    it->block_color.r = p->tool_color.r;
    it->block_color.g = p->tool_color.g;
    it->block_color.b = p->tool_color.b;
    it->health        = (uint8_t) (p->hp < 0 ? 0 : (p->hp > 255 ? 255 : p->hp));

    /* revision 1.1: the equipped jetpack and its thrust, so every client can
       predict and draw the flight; the thrust only while there is fuel, so
       the flames go out on the other clients when the tank runs dry */
    if (jetpack_equipped(p)) {
        it->flags |= P10_EF_JETPACK;
        if (p->jetpack_input && jetpack_fuel(p) > 0.0f) {
            it->input.flags |= P10_IN_JETPACK;
        }
    }
    /* a flier going up: the others' clients predict the climb */
    if (p->flying && p->fly_up) {
        it->input.flags |= P10_IN_JETPACK;
    }
}

/* a body's item goes out with the compact Player trajectory (17 bytes
   instead of 37: the item was 39-40 bytes, 99% of the upload).
   Every 1.0 client reads it, and the jetpack bits too: there is no older
   1.0 client to strip them for */
static void _compact(p10_entity_item* it)
{
    if (it->trajectory.type == P10_TRAJ_PLAYER) {
        it->trajectory.type = P10_TRAJ_PLAYER_COMPACT;
    }
}

static int _vec_eq(p10_vec3 a, p10_vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* fills out with the parts of cur that differ from last; 0 when nothing does */
static int _entity_delta(const p10_entity_item* last, const p10_entity_item* cur, p10_entity_item* out)
{
    *out           = *cur;
    out->mask      = 0;
    const p10_trajectory *a = &last->trajectory, *b = &cur->trajectory;
    if (last->flags != cur->flags)
        out->mask |= P10_EU_FLAGS;
    if (!_vec_eq(a->origin, b->origin) || !_vec_eq(a->velocity, b->velocity) || !_vec_eq(a->euler, b->euler))
        out->mask |= P10_EU_TRAJECTORY;
    if (memcmp(&last->input, &cur->input, sizeof cur->input) != 0)
        out->mask |= P10_EU_PLAYER_INPUT;
    if (last->tool != cur->tool)
        out->mask |= P10_EU_TOOL;
    if (memcmp(&last->block_color, &cur->block_color, sizeof cur->block_color) != 0)
        out->mask |= P10_EU_BLOCK_COLOR;
    if (last->health != cur->health)
        out->mask |= P10_EU_HEALTH;
    return out->mask != 0;
}

static void _full_player(server_t* server, player_t* p, p10_player_item* it)
{
    memset(it, 0, sizeof *it);
    it->player_id = p->id;
    it->mask      = P10_PU_CREATE | P10_PU_FLAGS | P10_PU_SCORE | P10_PU_TEAM | P10_PU_STATS | P10_PU_PING;
    /* the reference's isAdmin: the staff's badge on everyone's scoreboard */
    it->flags     = is_staff(server, p) ? P10_PF_ADMIN : 0;
    it->name      = p10_bytes_from_cstring(p->name);
    it->score     = p->kills + p->score;
    it->team      = p->team == TEAM_A ? P10_TEAM_1 : (p->team == TEAM_B ? P10_TEAM_2 : P10_TEAM_SPECTATOR);
    it->kills     = p->kills;
    it->deaths    = p->deaths;
    it->ping_ms   = conn_rtt(p->conn);
}

/* ---- sending ---- */

static void _send_to(player_t* receiver, const p10_writer* w)
{
    if (w->error || receiver->conn == NULL) {
        return;
    }
    conn_send_reliable(receiver->conn, w->data, w->size);
}

static int _is_10_in_game(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME;
}

static void _broadcast(server_t* server, const p10_writer* w)
{
    if (w->error) {
        LOG_ERROR("proto10: broadcast packet does not fit %d bytes", BRIDGE_BUF_SIZE);
        return;
    }
    player_t *  p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_is_10_in_game(p)) {
            continue;
        }
        conn_send_reliable(p->conn, w->data, w->size);
    }
}

static int _any_10_in_game(server_t* server)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_is_10_in_game(p)) {
            return 1;
        }
    }
    return 0;
}

/* ---- tick ---- */

static void _tick_players(server_t* server)
{
    p10_writer w;
    int        items = 0;
    uint64_t   now   = get_nanos();
    player_t * p, *tmp;

    p10_writer_init(&w, bridge_buf, sizeof bridge_buf);
    p10_start_player_update(&w);
    HASH_ITER(hh, server->players, p, tmp)
    {
        p10_player_item cur;
        if (!_announced(p)) {
            continue;
        }
        _full_player(server, p, &cur);
        if (!p->p10_announced) {
            p10_put_player_item(&w, &cur);
            items++;
            p->p10_announced    = 1;
            p->p10_last_team    = cur.team;
            p->p10_last_kills   = cur.kills;
            p->p10_last_deaths  = cur.deaths;
            p->p10_last_score   = cur.score;
            p->p10_last_ping    = cur.ping_ms;
            p->p10_last_flags   = cur.flags;
            p->p10_ping_sent_at = now;
            continue;
        }
        p10_player_item d = cur;
        d.mask            = 0;
        if (cur.flags != p->p10_last_flags) {
            d.mask |= P10_PU_FLAGS; /* a login or a logout */
            p->p10_last_flags = cur.flags;
        }
        if (cur.team != p->p10_last_team) {
            d.mask |= P10_PU_TEAM;
            p->p10_last_team = cur.team;
        }
        if (cur.kills != p->p10_last_kills || cur.deaths != p->p10_last_deaths || cur.score != p->p10_last_score) {
            d.mask |= P10_PU_SCORE | P10_PU_STATS;
            p->p10_last_kills  = cur.kills;
            p->p10_last_deaths = cur.deaths;
            p->p10_last_score  = cur.score;
        }
        if (cur.ping_ms != p->p10_last_ping && now - p->p10_ping_sent_at >= PING_RESEND_NANOS) {
            d.mask |= P10_PU_PING;
            p->p10_last_ping    = cur.ping_ms;
            p->p10_ping_sent_at = now;
        }
        if (d.mask) {
            p10_put_player_item(&w, &d);
            items++;
        }
    }
    if (items) {
        _broadcast(server, &w);
    }
}

/* What each 1.0 client has of each body: the last item it was sent and
   whether it has the body at all. Every client used to get every body's
   changes 60 times a second (one broadcast), ~140 KB/s each with 127 bots;
   now each gets the bodies near it at 60 Hz and the far ones less often
   (BRIDGE_RATE_*), each delta against what that client last got, so a
   skipped tick loses nothing */
#define BRIDGE_VIEW_MAX SERVER_MAX_PLAYERS
typedef struct {
    uint8_t         live;
    uint8_t         frozen; /* past the fog: sent standing still, then nothing */
    p10_entity_item last;
} bridge_view_t;
static bridge_view_t bridge_views[BRIDGE_VIEW_MAX][BRIDGE_VIEW_MAX];
static uint32_t      bridge_tick;
/* this tick's item of every live body, built once: built per receiving
   client it was 128 x 128 builds (with the orientation's trigonometry) a
   tick with a full server of people */
static p10_entity_item bridge_items[BRIDGE_VIEW_MAX];

/* until when a body is spotted (game time) and for which team: its mark
   follows the body on that team's screens and maps, wherever they are */
static uint64_t spot_until[BRIDGE_VIEW_MAX];
static uint8_t  spot_team[BRIDGE_VIEW_MAX];
#define SPOT_KEEP_NANOS (8ULL * NANO_IN_SECOND)

/* Past the fog (the client draws bodies within its fog, [server]
   fog_distance, and 2 more) an enemy is not seen: it gets one item standing
   still and then nothing, which was 30-44% of the bodies' bytes with 127
   bots. 8 blocks of margin over the fog. A teammate still moves
   on the minimap */
#define BRIDGE_FOG_CUT ((float) get_server()->fog_distance + 8.0f)
#define BRIDGE_PERIOD_TEAM 60 /* 1 Hz */
#define BRIDGE_PERIOD_FAR 12  /* 5 Hz */
#define BRIDGE_PERIOD_CUT 0

static float _dist2(float ax, float ay, float bx, float by)
{
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

/* every how many ticks a body this far from the receiver is updated;
   BRIDGE_PERIOD_CUT: not at all (an enemy past the fog) */
static int _entity_period(const player_t* receiver, const player_t* body)
{
    /* its own body; a spectator, whose client draws past the fog on 1.0
       (it sees the whole match); a player still joining. A dead
       player of a team keeps the cut: what it is not sent cannot leak to
       its team */
    if (receiver == body || receiver->team == TEAM_SPECTATOR || receiver->state != STATE_READY) {
        return 1;
    }
    float d2 = _dist2(body->movement.position.x, body->movement.position.y, receiver->movement.position.x,
                      receiver->movement.position.y);
    float fog = (float) get_server()->fog_distance;
    if (d2 < fog * fog) {
        return d2 < 32.0f * 32.0f ? 1 : d2 < 64.0f * 64.0f ? 2 : 4;
    }
    if (d2 < BRIDGE_FOG_CUT * BRIDGE_FOG_CUT) {
        return BRIDGE_PERIOD_FAR;
    }
    /* drawn past the fog: a squadmate's name over its head, a spotted
       enemy's mark, the intel its carrier holds (on the map), and a shot:
       the client learns of one from the trigger in the body's input, and
       a far shot is heard (a rifle's to 192 blocks) and its tracer drawn
       on the map */
    int mate    = body->team == receiver->team;
    int squad   = mate && body->squad != 0 && body->squad == receiver->squad;
    int spotted = body->id < BRIDGE_VIEW_MAX && spot_team[body->id] == receiver->team &&
                  get_nanos() < spot_until[body->id];
    if (squad || spotted || body->has_intel || body->primary_fire) {
        return BRIDGE_PERIOD_FAR;
    }
    return mate ? BRIDGE_PERIOD_TEAM : BRIDGE_PERIOD_CUT;
}

/* a body's health is for its owner and its team only: every client used to
   get everyone's, so a modified client read its enemies' (the 0.75
   extension sends them 0 too). A constant 0 makes no deltas */
static void _hide_health(const player_t* receiver, const player_t* body, p10_entity_item* it)
{
    if (receiver != body && (receiver->team != body->team || receiver->team == TEAM_SPECTATOR)) {
        it->health = 0;
    }
}

/* the receiver got everything in its GameStateFinal snapshot */
static void _views_from_snapshot(server_t* server, player_t* receiver)
{
    if (receiver->id >= BRIDGE_VIEW_MAX) {
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->id >= BRIDGE_VIEW_MAX) {
            continue;
        }
        bridge_view_t* v = &bridge_views[receiver->id][p->id];
        v->live          = (uint8_t) _entity_live(p);
        v->frozen        = 0;
        if (v->live) {
            _full_entity(p, &v->last);
            _hide_health(receiver, p, &v->last);
        }
    }
}

static void _tick_entities(server_t* server)
{
    player_t *p, *tmp, *r, *rtmp;
    bridge_tick++;
    /* bodies gone (death, spectator, leaving) and new ones: for everyone */
    HASH_ITER(hh, server->players, p, tmp)
    {
        uint8_t live = (uint8_t) _entity_live(p);
        /* died and back between two sends (a zombie's victim turns at once):
           the clients hold a corpse since the EntityDie, so the body goes and
           comes back with a Create, or they draw the corpse walking (and the
           victim's own client never gets its new body) */
        uint8_t reborn     = live && p->p10_entity_live && p->p10_entity_died;
        p->p10_entity_died = 0;
        if ((!live && p->p10_entity_live) || reborn) {
            uint8_t           rbuf[16];
            p10_writer        rw;
            p10_entity_remove rm = {p->id};
            p10_writer_init(&rw, rbuf, sizeof rbuf);
            p10_encode_entity_remove(&rw, &rm);
            _broadcast(server, &rw);
            if (reborn) {
                evt_log(p, "p10_entity", "action=reborn");
            }
        }
        if ((live != p->p10_entity_live || reborn) && p->id < BRIDGE_VIEW_MAX) {
            /* gone, or back with a Create for every client */
            for (int k = 0; k < BRIDGE_VIEW_MAX; k++) {
                bridge_views[k][p->id].live = 0;
            }
        }
        p->p10_entity_live = live;
        if (live && p->id < BRIDGE_VIEW_MAX) {
            _full_entity(p, &bridge_items[p->id]);
        }
    }
    HASH_ITER(hh, server->players, r, rtmp)
    {
        if (!_is_10_in_game(r) || r->id >= BRIDGE_VIEW_MAX) {
            continue;
        }
        p10_writer w;
        int        items  = 0;
        /* bodies created for r with their lamp on: the client turns a lamp
           off on a body's create, so the light goes again after it (a bot
           lights its lamp the tick it respawns, before its Create went) */
        uint8_t lit_new[BRIDGE_VIEW_MAX];
        int     n_lit_new = 0;
        p10_writer_init(&w, bridge_buf, sizeof bridge_buf);
        p10_start_entity_update(&w, 0);
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (!p->p10_entity_live || p->id >= BRIDGE_VIEW_MAX) {
                continue;
            }
            bridge_view_t*  v    = &bridge_views[r->id][p->id];
            p10_entity_item seen = bridge_items[p->id];
            _hide_health(r, p, &seen);
            const p10_entity_item* cur  = &seen;
            int                    band = NETSTATS_BAND_CREATE;
            p10_entity_item        out;
            p10_entity_item sent = *cur;
            if (!v->live) {
                out      = *cur;
                v->frozen = 0;
                if (p->light_on && n_lit_new < BRIDGE_VIEW_MAX) {
                    lit_new[n_lit_new++] = p->id;
                }
            } else {
                int period = _entity_period(r, p);
                band       = r == p ? NETSTATS_BAND_OWN
                             : period == 1  ? NETSTATS_BAND_32
                             : period == 2  ? NETSTATS_BAND_64
                             : period == 4  ? NETSTATS_BAND_128
                                            : NETSTATS_BAND_FAR;
                int freezing = 0;
                if (period == BRIDGE_PERIOD_CUT) {
                    /* the client's copy is where it was last sent: while that
                       is past the fog too, nothing; once the receiver comes
                       near it, it is updated to where the body really is */
                    const p10_vec3* seen = &v->last.trajectory.origin;
                    if (_dist2(seen->x, seen->y, r->movement.position.x, r->movement.position.y) <
                        BRIDGE_FOG_CUT * BRIDGE_FOG_CUT) {
                        period = BRIDGE_PERIOD_FAR;
                    } else if (v->frozen) {
                        continue;
                    } else {
                        /* last, standing still: the client walks a body on
                           with its keys, and a copy walking on could come
                           into view where the real one is not */
                        sent.trajectory.velocity = (p10_vec3) {0.0f, 0.0f, 0.0f};
                        sent.input.xmove = sent.input.ymove = 0;
                        sent.input.flags                    = 0;
                        _entity_delta(&v->last, &sent, &out);
                        out.mask |= P10_EU_TRAJECTORY | P10_EU_PLAYER_INPUT;
                        freezing = 1;
                    }
                }
                if (freezing) {
                    v->frozen = 1;
                } else {
                    if (v->frozen) {
                        v->frozen = 0; /* back in view: at once, from the still copy */
                    } else if ((bridge_tick + p->id) % (uint32_t) period != 0) {
                        continue;
                    }
                    if (!_entity_delta(&v->last, cur, &out)) {
                        continue;
                    }
                }
            }
            v->live = 1;
            v->last = sent;
            _compact(&out);
            size_t before = w.size;
            p10_put_entity_item(&w, &out);
            proto10_netstats_band(band, w.size - before);
            items++;
        }
        if (items) {
            _send_to(r, &w);
        }
        for (int k = 0; k < n_lit_new; k++) {
            char        id[8];
            snprintf(id, sizeof id, "%u", (unsigned) lit_new[k]);
            const char* parts[3] = {"light", id, "1"};
            proto10_bridge_generic(server, r, 3, parts);
        }
    }
}

/* A body changed in a way the per-tick delta cannot carry: a gun picked in
   place (the arena's countdown). The weapon parameters and skin only travel
   in a Create, so every receiver is made to take the body again; a respawn
   sends a Create the same way, and the client resets the body and reads the
   new gun and its full clip from it. */
void proto10_bridge_body_recreate(player_t* player)
{
    if (player == NULL || player->id >= BRIDGE_VIEW_MAX) {
        return;
    }
    for (int k = 0; k < BRIDGE_VIEW_MAX; k++) {
        bridge_views[k][player->id].live = 0;
    }
}

/* "host-status <bots> <humans> <autofill> <flags> <difficulty>" (docs/host-commands.md):
   the 1.0 form of PACKET_HOSTSTATUS, without which the client's bot menu
   says the server has no bot support. A per-tick diff, so autofill, joins
   and console commands all reach the menu without a hook in each of them. */
static void _tick_host_status(server_t* server)
{
    uint32_t  bots = 0, humans = 0, team_bots[2] = {0, 0};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot) {
            bots++;
            if (p->team < 2) {
                team_bots[p->team]++;
            }
        } else if (is_past_join_screen(p)) {
            humans++;
        }
    }
    uint8_t autofill   = bot_autofill_target(server);
    uint8_t difficulty = bot_difficulty();
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_is_10_in_game(p)) {
            p->p10_host_status = 0;
            continue;
        }
        uint8_t flags = (uint8_t) ((player_is_host(server, p) ? HOSTSTATUS_FLAG_CAN_MANAGE : 0) |
                                   (bots_enabled(server) ? HOSTSTATUS_FLAG_BOTS_ENABLED : 0) |
                                   (autofill > 0 ? HOSTSTATUS_FLAG_AUTOFILL : 0));
        /* the top bit keeps a sent status from ever reading as "never sent" */
        uint32_t status = 0x80000000u | (uint32_t) (bots & 0xFF) | (uint32_t) (humans & 0xFF) << 8 |
                          (uint32_t) autofill << 16 | (uint32_t) flags << 24 | (uint32_t) difficulty << 28;
        /* each team's bots apart ("host-bots <team1> <team2>"): a client
           keeps six parts of a command, host-status has them all */
        uint32_t teams = 0x80000000u | (team_bots[0] & 0xFF) | (team_bots[1] & 0xFF) << 8;
        if (teams != p->p10_host_bots) {
            char        t[2][8];
            snprintf(t[0], sizeof t[0], "%u", (unsigned) (team_bots[0] & 0xFF));
            snprintf(t[1], sizeof t[1], "%u", (unsigned) (team_bots[1] & 0xFF));
            const char* parts[] = {"host-bots", t[0], t[1]};
            proto10_bridge_generic(server, p, 3, parts);
            p->p10_host_bots = teams;
        }
        if (status == p->p10_host_status) {
            continue;
        }
        char       v[5][8];
        uint8_t    buf[64];
        p10_writer w;
        snprintf(v[0], sizeof v[0], "%u", (unsigned) (bots & 0xFF));
        snprintf(v[1], sizeof v[1], "%u", (unsigned) (humans & 0xFF));
        snprintf(v[2], sizeof v[2], "%u", (unsigned) autofill);
        snprintf(v[3], sizeof v[3], "%u", (unsigned) flags);
        snprintf(v[4], sizeof v[4], "%u", (unsigned) difficulty);
        p10_writer_init(&w, buf, sizeof buf);
        p10_start_generic_command(&w);
        p10_put_generic_part(&w, "host-status");
        for (int i = 0; i < 5; i++) {
            p10_put_generic_part(&w, v[i]);
        }
        _send_to(p, &w);
        p->p10_host_status = status;
    }
}

uint8_t proto10_net_frame = 1;

void proto10_bridge_tick(server_t* server)
{
    /* a sped up clock (SP_TIME_SCALE, tests): sends no more than 60 times a
       real second, which is all a client takes in (sent every tick, at 50x
       the client spent its frames draining packets and ENet dropped it);
       changes pile up meanwhile, the game itself runs every tick */
    if (get_time_scale() > 1.0) {
        static uint64_t sent_at;
        uint64_t        real = get_real_nanos();
        proto10_net_frame    = real - sent_at >= (uint64_t) NANO_60TPS;
        if (proto10_net_frame) {
            sent_at = real;
        }
    }
    if (proto10_net_frame) {
        proto10_gamemode_tick(server);
    }
    /* sentries think and perks advance whether or not a 1.0 client listens */
    proto10_objects_tick(server, _any_10_in_game(server) && proto10_net_frame);
    heli_tick(server);
    rockets_tick(server);
    drills_tick(server);
    crates_tick(server);
    perks_tick(server);
    if (proto10_net_frame) {
        _tick_host_status(server);
    }
    /* the delta state is global: with nobody to send to it must still advance,
       so a 1.0 client joining later starts from its GameStateFinal snapshot
       and then receives only real changes */
    if (!_any_10_in_game(server)) {
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            p->p10_announced   = 0;
            p->p10_entity_live = 0;
        }
        return;
    }
    if (!proto10_net_frame) {
        return;
    }
    _tick_players(server);
    _tick_entities(server);
}

/* the 1.0 flashlight goes off: death, spawn, team change, leave, map.
   Clients turn it off on the same events themselves; this keeps the
   state a joiner is told right. */
void proto10_bridge_light_off(player_t* player, const char* why)
{
    if (player == NULL || !player->light_on) {
        return;
    }
    player->light_on = 0;
    evt_log(player, "light", "action=off why=%s", why);
}

/* the lights already on, to a 1.0 client entering the game */
void proto10_bridge_lights_to(server_t* server, player_t* to)
{
    if (to == NULL || profile_is_075()) {
        return;
    }
    int         n = 0;
    player_t   *lit, *tmp;
    HASH_ITER(hh, server->players, lit, tmp)
    {
        if (!lit->light_on) {
            continue;
        }
        char        id[8];
        const char* parts[3];
        snprintf(id, sizeof id, "%u", lit->id);
        parts[0] = "light";
        parts[1] = id;
        parts[2] = "1";
        proto10_bridge_generic(server, to, 3, parts);
        n++;
    }
    if (n > 0) {
        evt_log(to, "light", "action=state lit=%d", n);
    }
}

void proto10_bridge_player_left(server_t* server, player_t* player)
{
    proto10_bridge_light_off(player, "leave");
    mount_player_left(server, player);
    outfit_player_left(player);
    if (!player->p10_announced) {
        return;
    }
    uint8_t    buf[16];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_player_remove(&w);
    p10_put_player_id(&w, player->id);
    _broadcast(server, &w);
    player->p10_announced   = 0;
    player->p10_entity_live = 0;
}

void proto10_bridge_put_snapshot(server_t* server, p10_writer* w, player_t* receiver)
{
    player_t *p, *tmp;
    uint32_t  entities = 0, players = 0;
    HASH_ITER(hh, server->players, p, tmp)
    {
        entities += (uint32_t) _entity_live(p);
        players += (uint32_t) _announced(p);
    }
    entities += proto10_entities_count(server) + proto10_objects_count() + proto10_flags_count() + crates_count();
    /* the joiner gets everything in full, and from here on the deltas of
       each body against what it got (_views_from_snapshot) */
    _views_from_snapshot(server, receiver);
    p10_put_count(w, entities);
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_entity_live(p)) {
            p10_entity_item it;
            _full_entity(p, &it);
            _hide_health(receiver, p, &it);
            _compact(&it);
            p10_put_entity_item(w, &it);
        }
    }
    proto10_entities_put(server, w);
    proto10_objects_put(w);
    proto10_flags_put(w);
    crates_put(w);
    p10_put_count(w, players);
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_announced(p)) {
            p10_player_item it;
            _full_player(server, p, &it);
            p10_put_player_item(w, &it);
        }
    }
}

/* ---- combat seen from 1.0 ---- */

/* the 0.75 kill reasons: 0 weapon, 1 headshot, 2 melee, 3 grenade, 4 fall,
   5 team change, 6 weapon change */
static uint8_t _damage_type_of_reason(uint8_t reason)
{
    switch (reason) {
        case 0:
        case 1:
            return P10_DMG_WEAPON1;
        case 2:
            return P10_DMG_MELEE;
        case 3:
            return P10_DMG_GRENADE;
        case 4:
            return P10_DMG_FALL;
        case KILL_REASON_SENTRY:
            return P10_DMG_SENTRY;
        case KILL_REASON_MG:
            return P10_DMG_WEAPON1; /* a round from the manned MG: a gun's hit */
        case KILL_REASON_EXPLOSIVE:
            return P10_DMG_GRENADE; /* a landmine or dynamite: a blast */
        default:
            return P10_DMG_SUICIDE;
    }
}

static p10_vec3 _vec(vector3f_t v)
{
    p10_vec3 r = {v.x, v.y, v.z};
    return r;
}

/* "hit <victim> <amount> <head>" / "kill <victim>" to the attacker: the
   shooter only learns that a shot landed from the server, so a hit marker
   never shows for a miss. Old clients ignore unknown commands. */
/* the reference's EntityEvent Jump: the others' clients jump the body at
   once instead of when its next trajectory comes (up to 1/15 s later in the
   middle distance), with its sound. Only to the clients that have the body */
void proto10_bridge_jump(server_t* server, player_t* player)
{
    if (profile_is_075() || player->id >= BRIDGE_VIEW_MAX) {
        return;
    }
    uint8_t          buf[32];
    p10_writer       w;
    p10_entity_event ev = {player->id, P10_EV_JUMP, 0};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_event(&w, &ev);
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != player && _is_10_in_game(p) && p->id < BRIDGE_VIEW_MAX && bridge_views[p->id][player->id].live &&
            !bridge_views[p->id][player->id].frozen)
        {
            _send_to(p, &w);
        }
    }
}

void proto10_bridge_reload(server_t* server, player_t* player)
{
    uint8_t          buf[32];
    p10_writer       w;
    p10_entity_event ev = {player->id, P10_EV_RELOAD_WEAPON, 0};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_event(&w, &ev);
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != player && _is_10_in_game(p)) {
            _send_to(p, &w);
        }
    }
}

static void _send_hit_confirm(player_t* attacker, player_t* victim, const char* kind, long amount, int head)
{
    if (attacker == NULL || attacker == victim || !_is_10_in_game(attacker)) {
        return;
    }
    uint8_t    buf[64];
    char       vid[16];
    char       num[16];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, kind);
    snprintf(vid, sizeof vid, "%u", (unsigned) victim->id);
    p10_put_generic_part(&w, vid);
    if (amount >= 0) {
        snprintf(num, sizeof num, "%ld", amount > 255 ? 255 : amount);
        p10_put_generic_part(&w, num);
        p10_put_generic_part(&w, head ? "1" : "0");
        evt_log(attacker, "hit_confirm", "victim=%u amount=%ld head=%d", (unsigned) victim->id, amount, head);
    } else {
        evt_log(attacker, "hit_confirm", "victim=%u kill=1", (unsigned) victim->id);
    }
    _send_to(attacker, &w);
}

void proto10_bridge_hit_protected(player_t* attacker, player_t* victim)
{
    if (attacker == NULL || attacker == victim || !_is_10_in_game(attacker)) {
        return;
    }
    uint8_t    buf[48];
    char       vid[16];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "hit-protected");
    snprintf(vid, sizeof vid, "%u", (unsigned) victim->id);
    p10_put_generic_part(&w, vid);
    _send_to(attacker, &w);
}

void proto10_bridge_kill(server_t* server, player_t* killer, player_t* victim, uint8_t reason, uint8_t respawn)
{
    proto10_bridge_light_off(victim, "death");
    mount_dismount(server, victim, "death");
    perks_on_kill(server, killer, victim, reason);
    crates_on_kill(server, killer, victim);
    _send_hit_confirm(killer, victim, "kill", -1, 0);
    victim->p10_entity_died = 1;
    if (!_any_10_in_game(server)) {
        return;
    }
    p10_entity_die die;
    memset(&die, 0, sizeof die);
    die.entity_id = victim->id;
    die.type      = P10_DEATH_PLAYER;
    /* the 0.75 reason travels as is, so a 1.0 kill feed can say exactly what
       a 0.75 one says (headshot, melee, team change...) */
    die.param                  = (uint64_t) reason | ((uint64_t) respawn << 8);
    die.has_damage             = 1;
    die.damage.damage_type     = _damage_type_of_reason(reason);
    /* a gun kill names the gun in hand (Weapon1 or Weapon2); a manned MG is
       not the gunner's own gun, so it stays Weapon1 */
    if (die.damage.damage_type == P10_DMG_WEAPON1 && killer != NULL && reason != KILL_REASON_MG) {
        die.damage.damage_type = (uint8_t) (P10_DMG_WEAPON1 + killer->gun_slot);
    }
    die.damage.has_from_entity = killer != NULL && killer != victim;
    die.damage.from_entity     = killer != NULL ? killer->id : victim->id;
    die.damage.fire_position   = _vec(killer != NULL ? killer->movement.position : victim->movement.position);
    die.damage.hit_position    = _vec(victim->movement.position);
    if ((reason == KILL_REASON_SENTRY || reason == KILL_REASON_EXPLOSIVE || reason == KILL_REASON_MG) &&
        proto10_objects_damage_source() != NULL)
    {
        die.damage.fire_position = _vec(proto10_objects_damage_source()->base);
    }

    uint8_t    buf[64];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_die(&w, &die);
    _broadcast(server, &w);
}

void proto10_bridge_damage(server_t*  server,
                           player_t*  attacker,
                           player_t*  victim,
                           long       amount,
                           uint8_t    type_of_damage,
                           uint8_t    is_grenade,
                           uint8_t    kill_reason,
                           vector3f_t source)
{
    (void) server;
    uint8_t head = kill_reason == 1;
    if (amount <= 0) {
        return;
    }
    /* before the victim check: the victim is often a bot, the shooter still
       wants the confirmation */
    if (type_of_damage != 0) {
        _send_hit_confirm(attacker, victim, "hit", amount, head);
    }
    if (!_is_10_in_game(victim)) {
        return;
    }
    p10_damage d;
    memset(&d, 0, sizeof d);
    d.entity_id                = victim->id;
    d.to_entity                = victim->id;
    d.amount                   = (uint8_t) (amount > 255 ? 255 : amount);
    /* the reference's damage types: a spade's or a claw's hit is Melee,
       a gun's the slot that fired it */
    d.damage.damage_type = type_of_damage == 0 ? P10_DMG_FALL
                         : is_grenade          ? P10_DMG_GRENADE
                         : kill_reason == 2    ? P10_DMG_MELEE
                         : attacker != NULL    ? (uint8_t) (P10_DMG_WEAPON1 + attacker->gun_slot)
                                               : P10_DMG_WEAPON1;
    d.damage.has_from_entity   = attacker != NULL && attacker != victim && type_of_damage != 0;
    d.damage.from_entity       = attacker != NULL ? attacker->id : 0;
    /* where the damage came from, for the hit direction indicator */
    d.damage.fire_position = _vec(is_grenade ? source : (attacker != NULL ? attacker->movement.position : source));
    d.damage.hit_position  = _vec(victim->movement.position);
    /* an object (sentry) hurting on its owner's behalf: the hit indicator
       points at the object */
    if (proto10_objects_damage_source() != NULL) {
        d.damage.damage_type   = P10_DMG_SENTRY;
        d.damage.fire_position = _vec(proto10_objects_damage_source()->base);
    }

    uint8_t    buf[64];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_damage(&w, &d);
    _send_to(victim, &w);
}

/* ---- packets from 1.0 clients ---- */

static void _send_local_player(player_t* player)
{
    uint8_t    buf[64];
    char       id[16];
    p10_writer w;
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "local-player");
    p10_put_generic_part(&w, id);
    _send_to(player, &w);
}

/* "join <team> <weapon>": replayed as the 0.75 ExistingPlayer a human sends
   after the team and weapon menus, so naming rules, team counts and the spawn
   flow are exactly the 0.75 ones */
static void _join(server_t* server, player_t* player, int team, int weapon, int klass)
{
    if (player->state != STATE_PROTO10) {
        evt_log(player, "proto10", "join_refused why=already_joined");
        return;
    }
    if ((team != P10_TEAM_1 && team != P10_TEAM_2 && team != P10_TEAM_SPECTATOR) || weapon < WEAPON_RIFLE ||
        weapon > GAME_WEAPON_ROCKET)
    {
        evt_log(player, "proto10", "join_refused why=bad_args team=%d weapon=%d", team, weapon);
        return;
    }
    /* one team for everyone (creative): there is nobody to be against */
    if (team == P10_TEAM_2 && gamemode_has(server, GM_FEAT_ONE_TEAM)) {
        team = P10_TEAM_1;
    }
    /* in a classes mode the class decides the gun, whatever the client
       named; elsewhere the class is ignored */
    if (classes_on(server)) {
        if (!classes_pick(server, player, klass, "join")) {
            classes_pick(server, player, classes_fallback(), "join");
        }
        weapon = classes_weapon(player->klass);
    } else if (weapon > WEAPON_SHOTGUN) {
        evt_log(player, "proto10", "join_refused why=bad_args team=%d weapon=%d", team, weapon);
        return;
    }
    /* the same call a 0.75 client's ExistingPlayer makes; the name is the
       one the handshake already applied */
    game_player_join(server,
                     player,
                     (uint8_t) (team == P10_TEAM_SPECTATOR ? TEAM_SPECTATOR : team),
                     (uint8_t) weapon,
                     TOOL_GUN,
                     0,
                     player->name);
    evt_log(player, "proto10", "join team=%d weapon=%d name=%s", team, weapon, player->name);
    _send_local_player(player);
    /* the client drops perk state until it knows its own id: resend it, or
       the HUD streak bar stays hidden until the first point */
    player->perk_sent = 0;
}

/* "ping <kind> <x> <y> <z> [n]" (map coordinates, z down; kind
   info|danger; n a phrase of common/pings/pings.h, the client's ring): a
   mark in the world for the player's team, relayed as "ping <player> <kind>
   <x> <y> <z> [n]" to its other members. The phrase goes as its number, so
   each client shows it in its own language and no free text lands on a
   marker. The sender drew it already. At most one info ping every 0.2 s;
   the danger ping a quick second press makes of it always goes through (the
   first version refused it as too fast) */
static void _ping(server_t* server, player_t* player, p10_bytes* parts, int n)
{
    static uint64_t last[256], last_danger[256];
    char            kind[8], c[3][16], ph[8] = "";
    uint64_t        now = get_nanos();
    if (!p10_bytes_copy(parts[1], kind, sizeof kind) || !p10_bytes_copy(parts[2], c[0], sizeof c[0]) ||
        !p10_bytes_copy(parts[3], c[1], sizeof c[1]) || !p10_bytes_copy(parts[4], c[2], sizeof c[2]) ||
        (n > 5 && !p10_bytes_copy(parts[5], ph, sizeof ph)))
    {
        return;
    }
    float x = strtof(c[0], NULL), y = strtof(c[1], NULL), z = strtof(c[2], NULL);
    char* end   = NULL;
    long  phrase = *ph ? strtol(ph, &end, 10) : -1;
    const char* why = NULL;
    if (player->is_bot) {
        why = "bot"; /* pings are for people talking to people */
    } else if (strcmp(kind, "info") != 0 && strcmp(kind, "danger") != 0) {
        why = "kind";
    } else if (*ph && (*end != '\0' || game_ping_phrase((int) phrase) == NULL)) {
        why = "phrase";
    } else if (player->team != TEAM_A && player->team != TEAM_B) {
        why = "spectator";
    } else if (!(x >= 0.0f && y >= 0.0f && z >= 0.0f && x < server->s_map.map.size_x &&
                 y < server->s_map.map.size_y && z < server->s_map.map.size_z))
    {
        why = "position";
    } else if (strcmp(kind, "info") == 0 && now - last[player->id] < NANO_IN_MILLI * 200) {
        why = "rate";
    } else if (strcmp(kind, "danger") == 0 && now - last_danger[player->id] < NANO_IN_MILLI * 200) {
        why = "rate";
    }
    if (why != NULL) {
        evt_log(player, "ping", "refused kind=%s why=%s", kind, why);
        return;
    }
    if (strcmp(kind, "danger") == 0) {
        last_danger[player->id] = now;
    } else {
        last[player->id] = now;
    }
    char id[8], pos[3][16];
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    snprintf(pos[0], sizeof pos[0], "%.1f", x);
    snprintf(pos[1], sizeof pos[1], "%.1f", y);
    snprintf(pos[2], sizeof pos[2], "%.1f", z);
    uint8_t    buf[96];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "ping");
    p10_put_generic_part(&w, id);
    p10_put_generic_part(&w, kind);
    p10_put_generic_part(&w, pos[0]);
    p10_put_generic_part(&w, pos[1]);
    p10_put_generic_part(&w, pos[2]);
    if (*ph) {
        snprintf(ph, sizeof ph, "%ld", phrase);
        p10_put_generic_part(&w, ph);
    }
    int       sent = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != player && p->team == player->team && _is_10_in_game(p)) {
            _send_to(p, &w);
            sent++;
        }
    }
    evt_log(player,
            "ping",
            "kind=%s at=%.1f,%.1f,%.1f phrase=%ld team=%u sent=%d",
            kind,
            x,
            y,
            z,
            phrase,
            player->team,
            sent);
}

/* "light <0|1>": the 1.0 flashlight, relayed as "light <player> <0|1>" to
   every 1.0 client (the owner too: a client draws only a light the server
   switched on; everyone, not only the bodies in view, because it is one
   boolean and a body coming back from past the fog must already know it).
   Turned off on death, spawn, team change, leave and map
   (proto10_bridge_light_off). */
static void _light(server_t* server, player_t* player, p10_bytes* parts)
{
    char        v[4];
    int         on  = 0;
    uint64_t    now;
    const char* why = NULL;
    if (!p10_bytes_copy(parts[1], v, sizeof v) || (strcmp(v, "0") != 0 && strcmp(v, "1") != 0)) {
        evt_log(player, "light", "action=refused on=%d why=value", on);
        return;
    }
    on  = v[0] == '1';
    now = get_nanos();
    if (player->is_bot) {
        why = "bot";
    } else if (player->team != TEAM_A && player->team != TEAM_B) {
        why = "team";
    } else if (player->state != STATE_READY || !player->alive) {
        why = "dead";
    } else if (on && (gamemode_player_rules(server, player) & GM_RULE_NO_LIGHT)) {
        why = "mode"; /* a zombie carries no light */
    } else if (on == player->light_on) {
        why = "same";
    } else if (now - player->light_at < NANO_IN_MILLI * 200) {
        why = "rate";
    } else if (!plugins_on_flashlight(server, player, on)) {
        why = "plugin";
    }
    if (why != NULL) {
        evt_log(player, "light", "action=refused on=%d why=%s", on, why);
        return;
    }
    player->light_on = (uint8_t) on;
    player->light_at = now;
    char        id[8];
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    const char* parts_out[3] = {"light", id, on ? "1" : "0"};
    proto10_bridge_generic(server, NULL, 3, parts_out);
    evt_log(player, "light", "action=%s why=request", on ? "on" : "off");
}

/* a bot's light (Bots.c, bots_lights): on in the dark, off by day; the
   same flag and relay as a player's request, and the plugins' say */
int proto10_bridge_bot_light(server_t* server, player_t* bot, int on, const char* why)
{
    if (on == bot->light_on || !bot->alive || bot->state != STATE_READY) {
        return 0;
    }
    if (on && ((gamemode_player_rules(server, bot) & GM_RULE_NO_LIGHT) || !plugins_on_flashlight(server, bot, on))) {
        return 0;
    }
    bot->light_on = (uint8_t) on;
    bot->light_at = get_nanos();
    char        id[8];
    snprintf(id, sizeof id, "%u", (unsigned) bot->id);
    const char* parts_out[3] = {"light", id, on ? "1" : "0"};
    proto10_bridge_generic(server, NULL, 3, parts_out);
    evt_log(bot, "light", "action=%s why=%s", on ? "on" : "off", why);
    return 1;
}

/* "device <platform> <input>": what the client plays on, relayed
   as "device <player> <platform> <input>" to every 1.0 client for the
   scoreboard's icon. The words come from common/devices/devices.h; the
   command budget (Flood.c) is the rate limit, and an unchanged one is
   dropped */
static void _device(server_t* server, player_t* player, p10_bytes* parts, int n)
{
    char pw[12], iw[12], fw[16];
    int  platform = -1, input = -1, family = 0;
    /* the family is optional on the wire: an older client sends three parts */
    if (p10_bytes_copy(parts[1], pw, sizeof pw) && p10_bytes_copy(parts[2], iw, sizeof iw)) {
        platform = game_device_platform_find(pw);
        input    = game_device_input_find(iw);
        if (n >= 4 && p10_bytes_copy(parts[3], fw, sizeof fw)) {
            int f = game_device_family_find(fw);
            if (f >= 0) {
                family = f;
            }
        }
    }
    if (platform < 0 || input < 0) {
        evt_log(player, "device", "action=refused why=value");
        return;
    }
    /* a non-pad input has no family */
    if (input != GAME_INPUT_PAD) {
        family = GAME_DEVICE_FAMILY_UNKNOWN;
    }
    if (platform == player->device_platform && input == player->device_input
        && family == player->device_family) {
        return;
    }
    player->device_platform = (uint8_t) platform;
    player->device_input    = (uint8_t) input;
    player->device_family   = (uint8_t) family;
    char id[8];
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    const char* out[5] = {"device", id, game_device_platform(platform), game_device_input(input),
                          game_device_family(family)};
    proto10_bridge_generic(server, NULL, 5, out);
    evt_log(player, "device", "action=set platform=%s input=%s family=%s", out[2], out[3], out[4]);
}

/* the devices already known, to a 1.0 client entering the game */
void proto10_bridge_devices_to(server_t* server, player_t* to)
{
    if (to == NULL || profile_is_075()) {
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->device_platform == 0) {
            continue;
        }
        char id[8];
        snprintf(id, sizeof id, "%u", (unsigned) p->id);
        const char* out[5] = {"device", id, game_device_platform(p->device_platform),
                              game_device_input(p->device_input), game_device_family(p->device_family)};
        proto10_bridge_generic(server, to, 5, out);
    }
}

/* "ping spot <target> x y z": 3D spotting (BF4). The
   enemy under the pinger's crosshair is marked for its team for
   SPOT_SECONDS and the mark follows it. Only where the mode has
   GM_FEAT_SPOTTING (ngCTF), never through a wall: the pinger's
   eye must see the target's feet, chest or head, the bots' own sight test.
   The relay carries the server's position, not the pinger's. */
#define SPOT_SECONDS 8
static void _spot(server_t* server, player_t* player, p10_bytes* parts)
{
    static uint64_t last[256];
    char            tid[8];
    uint64_t        now    = get_nanos();
    player_t*       target = NULL;
    const char*     why    = NULL;
    if (p10_bytes_copy(parts[2], tid, sizeof tid)) {
        uint8_t id = (uint8_t) atoi(tid);
        HASH_FIND(hh, server->players, &id, sizeof(id), target);
    }
    if (player->is_bot) {
        why = "bot";
    } else if (!gamemode_has(server, GM_FEAT_SPOTTING)) {
        why = "mode";
    } else if (player->team != TEAM_A && player->team != TEAM_B) {
        why = "spectator";
    } else if (target == NULL || target->state != STATE_READY || !target->alive) {
        why = "target";
    } else if (target->team == player->team || (target->team != TEAM_A && target->team != TEAM_B)) {
        why = "team";
    } else if (now - last[player->id] < NANO_IN_MILLI * 500) {
        why = "rate";
    } else {
        static const float part[3] = {0.1f, 0.9f, 1.7f};
        int                seen    = 0;
        for (int k = 0; k < 3 && !seen; k++) {
            seen = physics_can_see(server, player->movement.position.x, player->movement.position.y,
                                   player->movement.position.z, target->movement.position.x,
                                   target->movement.position.y, target->movement.position.z + part[k]) != 0;
        }
        if (!seen) {
            why = "not_visible";
        }
    }
    if (why != NULL) {
        evt_log(player, "spot", "refused target=%s why=%s", tid, why);
        /* the client falls back to a plain ping where the mode has no spotting */
        if (!player->is_bot) {
            const char* reply[] = {"spot-refused", why};
            proto10_bridge_generic(server, player, 2, reply);
        }
        return;
    }
    last[player->id] = now;
    if (target->id < BRIDGE_VIEW_MAX) {
        spot_until[target->id] = now + SPOT_KEEP_NANOS;
        spot_team[target->id]  = player->team;
    }
    char id[8], pos[3][16];
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    snprintf(tid, sizeof tid, "%u", (unsigned) target->id);
    snprintf(pos[0], sizeof pos[0], "%.1f", target->movement.position.x);
    snprintf(pos[1], sizeof pos[1], "%.1f", target->movement.position.y);
    snprintf(pos[2], sizeof pos[2], "%.1f", target->movement.position.z);
    uint8_t    buf[96];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "spot");
    p10_put_generic_part(&w, id);
    p10_put_generic_part(&w, tid);
    p10_put_generic_part(&w, pos[0]);
    p10_put_generic_part(&w, pos[1]);
    p10_put_generic_part(&w, pos[2]);
    int       sent = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        /* the pinger too: its own mark is drawn when the server agrees */
        if (p->team == player->team && _is_10_in_game(p)) {
            _send_to(p, &w);
            sent++;
        }
    }
    evt_log(player, "spot", "target=%u at=%.1f,%.1f,%.1f sent=%d", (unsigned) target->id,
            target->movement.position.x, target->movement.position.y, target->movement.position.z, sent);
}

/* a mark the mode puts on a body (the arena's last survivors): what a spot
   of a player's is, but the mode decides when, so no sight test and no rate.
   The packet's pinger is the receiver itself: no horn at every refresh, and
   the name drawn is the marked body's anyway */
void proto10_bridge_spot_mark(server_t* server, player_t* target, uint8_t team)
{
    if (target == NULL || target->state != STATE_READY || !target->alive) {
        return;
    }
    uint64_t now = get_nanos();
    if (target->id < BRIDGE_VIEW_MAX) {
        /* the mark keeps the body's updates coming, as a player's spot does */
        spot_until[target->id] = now + SPOT_KEEP_NANOS;
        spot_team[target->id]  = team;
    }
    char tid[8], pos[3][16];
    snprintf(tid, sizeof tid, "%u", (unsigned) target->id);
    snprintf(pos[0], sizeof pos[0], "%.1f", target->movement.position.x);
    snprintf(pos[1], sizeof pos[1], "%.1f", target->movement.position.y);
    snprintf(pos[2], sizeof pos[2], "%.1f", target->movement.position.z);
    int       sent = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team != team || !_is_10_in_game(p)) {
            continue;
        }
        char       pid[8];
        uint8_t    buf[96];
        p10_writer w;
        snprintf(pid, sizeof pid, "%u", (unsigned) p->id);
        p10_writer_init(&w, buf, sizeof buf);
        p10_start_generic_command(&w);
        p10_put_generic_part(&w, "spot");
        p10_put_generic_part(&w, pid);
        p10_put_generic_part(&w, tid);
        for (int k = 0; k < 3; k++) {
            p10_put_generic_part(&w, pos[k]);
        }
        _send_to(p, &w);
        sent++;
    }
    evt_log(target, "spot", "action=mark_by_mode team=%u sent=%d", (unsigned) team, sent);
}

static void _on_generic(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_reader r;
    p10_bytes  parts[8];
    int        n = 0;
    /* over the command budget: dropped unparsed (Flood.c) */
    if (!flood_take(server, player, FLOOD_COMMAND)) {
        return;
    }
    if (!p10_begin_generic_command(&r, data, size)) {
        *ok = 0;
        return;
    }
    p10_bytes part;
    while (p10_next_generic_part(&r, &part)) {
        if (n < 8) {
            parts[n] = part;
        }
        n++;
    }
    if (r.error || n == 0) {
        *ok = r.error ? 0 : 1;
        return;
    }
    char arg1[8] = "", arg2[8] = "";
    /* "join <team> <weapon> [<class>]": the class is a fourth part (old
       clients send three: the Commando) */
    char arg4[8] = "";
    if (p10_bytes_equal(parts[0], "join") && (n == 3 || n == 4) && p10_bytes_copy(parts[1], arg1, sizeof arg1) &&
        p10_bytes_copy(parts[2], arg2, sizeof arg2) && (n == 3 || p10_bytes_copy(parts[3], arg4, sizeof arg4)))
    {
        _join(server, player, atoi(arg1), atoi(arg2), n == 4 ? atoi(arg4) : GAME_CLASS_COMMANDO);
        return;
    }
    /* "team <n>" and "weapon <n>": the team and weapon screens in the game,
       replayed into the 0.75 handlers like join (they were not handled: a
       player could not go to the spectators or change its gun) */
    /* the reference's "leave": remove my body, back to spectating (the same
       as "team 255", which stays for the team screen) */
    if (p10_bytes_equal(parts[0], "leave") && n == 1) {
        if (player->state == STATE_PROTO10) {
            evt_log(player, "proto10", "team_refused why=not_joined");
            return;
        }
        evt_log(player, "proto10", "leave");
        game_player_change_team(server, player, TEAM_SPECTATOR);
        return;
    }
    if ((p10_bytes_equal(parts[0], "team") || p10_bytes_equal(parts[0], "weapon")) && n == 2 &&
        p10_bytes_copy(parts[1], arg1, sizeof arg1))
    {
        int      team  = p10_bytes_equal(parts[0], "team");
        int      value = atoi(arg1);
        if (player->state == STATE_PROTO10 || value < 0 || value > 255) {
            evt_log(player, "proto10", "%s_refused why=%s", team ? "team" : "weapon",
                    player->state == STATE_PROTO10 ? "not_joined" : "bad_value");
            return;
        }
        /* the class decides the gun: "class <n>" changes it */
        if (!team && classes_on(server)) {
            evt_log(player, "proto10", "weapon_refused why=classes value=%d", value);
            return;
        }
        if (team && value == P10_TEAM_2 && gamemode_has(server, GM_FEAT_ONE_TEAM)) {
            evt_log(player, "proto10", "team_refused why=one_team");
            return;
        }
        evt_log(player, "proto10", "%s value=%d", team ? "team" : "weapon", value);
        if (team) {
            game_player_change_team(server, player, (uint8_t) (value == P10_TEAM_SPECTATOR ? TEAM_SPECTATOR : value));
        } else {
            game_player_change_weapon(server, player, (uint8_t) value);
        }
        return;
    }
    /* "host <command> <arg1> <arg2>": PACKET_HOSTCMD over 1.0, into the same
       queue as the console, which checks the permission */
    char arg3[8] = "";
    if (p10_bytes_equal(parts[0], "host") && n == 4 && p10_bytes_copy(parts[1], arg1, sizeof arg1) &&
        p10_bytes_copy(parts[2], arg2, sizeof arg2) && p10_bytes_copy(parts[3], arg3, sizeof arg3))
    {
        evt_log(player, "host", "command=%s arg1=%s arg2=%s", arg1, arg2, arg3);
        hostcmd_apply(server, player, (uint8_t) atoi(arg1), (uint8_t) atoi(arg2), (uint8_t) atoi(arg3));
        return;
    }
    if (p10_bytes_equal(parts[0], "light") && n == 2) {
        _light(server, player, parts);
        return;
    }
    /* "vote <index>": the next map's vote, held while the match's summary is
       up (MapVote.c) */
    if (p10_bytes_equal(parts[0], "vote") && n == 2 && p10_bytes_copy(parts[1], arg1, sizeof arg1)) {
        mapvote_cast(server, player, atoi(arg1));
        return;
    }
    /* "use": board the nearest thing in reach, or leave what we are on
       (Proto10/Mount.c; the helicopter's seats and the MG) */
    if (p10_bytes_equal(parts[0], "use") && n == 1) {
        mount_use(server, player);
        return;
    }
    if (p10_bytes_equal(parts[0], "device") && (n == 3 || n == 4)) {
        _device(server, player, parts, n);
        return;
    }
    if (p10_bytes_equal(parts[0], "ping") && (n == 5 || (n == 6 && !p10_bytes_equal(parts[1], "spot")))) {
        _ping(server, player, parts, n);
        return;
    }
    if (p10_bytes_equal(parts[0], "ping") && n == 6 && p10_bytes_equal(parts[1], "spot")) {
        _spot(server, player, parts);
        return;
    }
    char cmd[32];
    if (!p10_bytes_copy(parts[0], cmd, sizeof cmd)) {
        snprintf(cmd, sizeof cmd, "?");
    }
    /* the mode's own requests ("squad <n>") */
    if (n <= 2 && (n == 1 || p10_bytes_copy(parts[1], arg1, sizeof arg1)) &&
        gamemode_player_command(server, player, cmd, n == 2 ? arg1 : ""))
    {
        return;
    }
    /* unknown commands are ignored by design (docs/protocol-1.0.md 10) */
    evt_log(player, "proto10", "command_ignored cmd=%s parts=%d", cmd, n);
}

static void _replay_input(server_t* server, player_t* player, const p10_player_input* in, int jump)
{
    uint8_t keys = 0;
    keys |= (in->ymove > 0) ? 1 << 0 : 0;
    keys |= (in->ymove < 0) ? 1 << 1 : 0;
    keys |= (in->xmove < 0) ? 1 << 2 : 0;
    keys |= (in->xmove > 0) ? 1 << 3 : 0;
    keys |= jump ? 1 << 4 : 0;
    keys |= (in->stance == P10_STANCE_CROUCH) ? 1 << 5 : 0;
    keys |= (in->flags & P10_IN_SPRINT) ? 1 << 7 : 0;
    /* the reference's analogue xmove/ymove: a stick pushed half way walks
       at half the push (the client predicts the same, player_move) */
    int ax = in->xmove < 0 ? -in->xmove : in->xmove, ay = in->ymove < 0 ? -in->ymove : in->ymove;
    int am = ax > ay ? ax : ay;
    player->move_scale = am > 0 ? (am >= 127 ? 1.0f : (float) am / 127.0f) : 1.0f;
    if (keys != player->input || jump) {
        game_player_move_input(server, player, keys);
    }
    uint8_t fire = (uint8_t) (((in->flags & P10_IN_TOOL_PRIMARY) ? 1 : 0) | ((in->flags & P10_IN_TOOL_SECONDARY) ? 2 : 0));
    if (mount_mounted(player)) {
        /* nothing of the rider's own fires: the MG gunner's trigger fires the
           MG (Mg.c reads primary_fire) and a helicopter's occupants have no
           gun at all (12.8) */
        player->primary_fire   = fire & 1;
        player->secondary_fire = (fire >> 1) & 1;
        return;
    }
    if ((fire & 1) != player->primary_fire || ((fire >> 1) & 1) != player->secondary_fire) {
        game_player_fire_input(server, player, fire);
    }
}

/* the server refused the reported position: correct the owner, the way the
   0.75 protocol answered with a PositionData */
static void _send_correction(player_t* player)
{
    p10_writer      w;
    p10_entity_item it;
    _full_entity(player, &it);
    it.mask = P10_EU_TRAJECTORY;
    p10_writer_init(&w, bridge_buf, sizeof bridge_buf);
    p10_start_entity_update(&w, 1);
    p10_put_entity_item(&w, &it);
    _send_to(player, &w);
}

void proto10_bridge_teleport(player_t* player)
{
    if (profile_is_075()) { /* the 0.75 profile's wire (Legacy/Wire075.c) */
        legacy_send_position(player, player->movement.position.x, player->movement.position.y,
                             player->movement.position.z);
        return;
    }
    _send_correction(player);
}

void proto10_bridge_wallet(server_t* server, player_t* player)
{
    if (player == NULL || player->conn == NULL || profile_is_075()) {
        return;
    }
    char        n[8];
    const char* parts[] = {"wallet", n};
    snprintf(n, sizeof n, "%u", (unsigned) player->blocks);
    proto10_bridge_generic(server, player, 2, parts);
}

void proto10_bridge_generic(server_t* server, player_t* to, int n, const char* const* parts)
{
    if (profile_is_075()) { /* the 0.75 profile's wire (Legacy/Wire075.c) */
        legacy_generic(server, to, n, parts);
        return;
    }
    p10_writer w;
    p10_writer_init(&w, bridge_buf, sizeof bridge_buf);
    p10_start_generic_command(&w);
    for (int k = 0; k < n; k++) {
        p10_put_generic_part(&w, parts[k]);
    }
    if (to == NULL) {
        _broadcast(server, &w);
    } else if (_is_10_in_game(to)) {
        _send_to(to, &w);
    }
}

/* ngspades' way to fire: the trigger (PlayerInput's toolPrimary) with the
   weapon in hand, the server firing from its own copy of the eye and the
   look, which this update has just brought (the client forces it, reliable,
   at the press). Last in _apply_own_entity, so a tool changed by the same
   update counts. The rocket launcher and the Miner's drill fire so; the
   hitscan guns still report their hits (HitEntity, HitTerrain) */
static void _fire_on_press(server_t* server, player_t* player)
{
    /* a seat fires the seat's gun: the MG's own tick, never the tool in hand */
    if (mount_mounted(player)) {
        return;
    }
    if (player->item == TOOL_GUN && player->weapon == GAME_WEAPON_ROCKET) {
        vector3f_t dir = player->movement.forward_orientation;
        /* a block ahead of the eye, where the client draws the launch */
        vector3f_t origin = {player->movement.eye_pos.x + dir.x, player->movement.eye_pos.y + dir.y,
                             player->movement.eye_pos.z + dir.z};
        rockets_fire(server, player, origin, dir, "input");
        return;
    }
    const struct game_class_info* c = classes_of(server, player);
    /* the drill sits in the grenade slot; a class with grenades throws them
       with the same trigger (ThrowGrenade), so only the drill's class */
    if (player->item == TOOL_GRENADE && c != NULL && c->gear == GAME_GEAR_DRILL) {
        vector3f_t look = {0, 0, 0};
        classes_drill(server, player, look, "input");
    }
}

static void _apply_own_entity(server_t* server, player_t* player, const p10_entity_item* it)
{
    if (it->mask & P10_EU_TRAJECTORY) {
        const p10_trajectory* t = &it->trajectory;
        if (t->type != P10_TRAJ_PLAYER) {
            evt_log(player, "proto10", "update_refused why=trajectory_type type=%hhu", t->type);
            _send_correction(player);
            return;
        }
        /* a seat holds the body: the seat's position wins, the look does not
           (the pilot's heading and the MG gunner's aim read it) */
        if (!mount_mounted(player)) {
            game_player_move(server, player, (vector3f_t) {t->origin.x, t->origin.y, t->origin.z});
            if (player->movement.position.x != t->origin.x || player->movement.position.y != t->origin.y ||
                player->movement.position.z != t->origin.z)
            {
                evt_log(player, "proto10", "position_corrected");
                _send_correction(player);
            }
        }

        p10_vec3 f = p10_front_from_euler(t->euler);
        game_player_look(server, player, (vector3f_t) {f.x, f.y, f.z});
    }
    if (it->mask & P10_EU_PLAYER_INPUT) {
        /* the jetpack bit has no 0.75 equivalent: it bypasses the replay.
           Flying (creative) it is the held jump: up. On a seat the same bit
           is the helicopter's lift (Mount.c), never the jetpack */
        if (mount_mounted(player)) {
            /* its own fuel stays out of it */
        } else if (player->flying) {
            player->fly_up = (it->input.flags & P10_IN_JETPACK) != 0;
        } else {
            jetpack_set_input(server, player, (it->input.flags & P10_IN_JETPACK) != 0);
        }
        _replay_input(server, player, &it->input, 0);
    }
    if (it->mask & P10_EU_TOOL) {
        uint8_t tool;
        int     gun;
        if (_tool_from_p10(it->tool, &tool, &gun)) {
            /* the tool slot names the gun: put the wanted one in hand first
               (refused with no second gun, the whole change is dropped) */
            if (gun < 0 || classes_select_gun(server, player, gun, "client")) {
                game_player_set_tool(server, player, tool);
            } else {
                evt_log(player, "proto10", "tool_refused why=no_second_gun slot=1");
            }
        }
    }
    if (it->mask & P10_EU_BLOCK_COLOR) {
        if (it->block_color.r != player->tool_color.r || it->block_color.g != player->tool_color.g ||
            it->block_color.b != player->tool_color.b)
        {
            /* the 0.75 colour is on the wire as b, g, r */
            color_t color;
            color.b = it->block_color.b;
            color.g = it->block_color.g;
            color.r = it->block_color.r;
            color.a = 0;
            game_player_set_color(server, player, color);
        }
    }
    if (it->mask & P10_EU_PLAYER_INPUT) {
        /* the reference's chat bit: the chat box is open, the others show it */
        player->typing        = (it->input.flags & P10_IN_CHAT) != 0;
        uint8_t pressed       = (it->input.flags & P10_IN_TOOL_PRIMARY) && !(player->p10_input_prev & P10_IN_TOOL_PRIMARY);
        player->p10_input_prev = it->input.flags;
        if (pressed) {
            _fire_on_press(server, player);
        }
    }
}

static void _on_client_entity_update(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_reader      r;
    p10_entity_item it;
    if (!p10_begin_client_entity_update(&r, data, size)) {
        *ok = 0;
        return;
    }
    while (p10_next_entity_item(&r, &it)) {
        /* a client owns exactly one entity, its body, and cannot create any */
        if (it.entity_id != player->id || (it.mask & P10_EU_CREATE)) {
            evt_log(player, "proto10", "update_refused why=not_owner entity=%u", it.entity_id);
            continue;
        }
        if (player->state != STATE_READY || !player->alive) {
            continue;
        }
        _apply_own_entity(server, player, &it);
    }
    if (r.error) {
        *ok = 0;
    }
}

static void _on_player_action(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_player_action a;
    if (!p10_decode_player_action(data, size, &a)) {
        *ok = 0;
        return;
    }
    if (player->state != STATE_READY || !player->alive) {
        return;
    }
    if (a.type == P10_EV_RELOAD_WEAPON) {
        game_player_reload(server, player);
        return;
    }
    if (a.type == P10_EV_USE_PERK) {
        perks_on_use(server, player, a.param);
        return;
    }
    if (a.type == P10_EV_JUMP) {
        p10_player_input in;
        memset(&in, 0, sizeof in);
        in.ymove  = (int8_t) (127 * ((int) player->move_forward - (int) player->move_backwards));
        in.xmove  = (int8_t) (127 * ((int) player->move_right - (int) player->move_left));
        in.stance = player->crouching ? P10_STANCE_CROUCH : P10_STANCE_STAND;
        in.flags  = player->sprinting ? P10_IN_SPRINT : 0;
        /* a zombie's charged leap: how long it held the button. Kept to the
           next physics step (Player.c), which leaps: the client leapt from
           the ground it stands on, ours lags a ping behind, and the body
           input of the same tick cleared the jump */
        player->leap_charge = (gamemode_player_rules(server, player) & GM_RULE_HIGH_JUMP)
                              ? (uint8_t) (a.param > 255 ? 255 : a.param)
                              : 0;
        _replay_input(server, player, &in, 1);
    } else if (a.type == P10_EV_SUICIDE) {
        /* the reference's own action for /kill: the pause menu's Respawn, so
           a stuck player on a pad needs no typing. The same death as /kill */
        if (player->state != STATE_READY || !player->alive || player->team == TEAM_SPECTATOR) {
            evt_log(player, "proto10", "suicide_refused why=not_alive");
            return;
        }
        evt_log(player, "proto10", "suicide");
        send_kill_action_packet(server, player, player, 0, 5, 0);
    } else {
        evt_log(player, "proto10", "action_ignored type=%hhu", a.type);
    }
}

/* the shot is still judged by the 0.75 allow_shot(); the 1.0 packet adds where
   it was fired from, so a client claiming hits from far away is refused */
static void _on_hit_entity(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_hit_entity h;
    if (!p10_decode_hit_entity(data, size, &h)) {
        *ok = 0;
        return;
    }
    if (player->state != STATE_READY || !player->alive) {
        return;
    }
    if (h.entity_id > 255) {
        proto10_objects_on_hit(server, player, &h);
        return;
    }
    float dx = h.fire_position.x - player->movement.eye_pos.x;
    float dy = h.fire_position.y - player->movement.eye_pos.y;
    float dz = h.fire_position.z - player->movement.eye_pos.z;
    if (dx * dx + dy * dy + dz * dz > 4.0f * 4.0f) {
        evt_log(player, "proto10", "hit_refused why=fire_position_far entity=%u", h.entity_id);
        return;
    }
    /* a hit only counts for the gun the player has in hand (Weapon1 or
       Weapon2): the slot is one server state, not a client's choice */
    if ((h.damage_type == P10_DMG_WEAPON1 || h.damage_type == P10_DMG_WEAPON2) &&
        h.damage_type != P10_DMG_WEAPON1 + player->gun_slot) {
        evt_log(player, "proto10", "hit_refused why=slot entity=%u type=%u slot=%u", h.entity_id,
                (unsigned) h.damage_type, (unsigned) player->gun_slot);
        return;
    }
    /* the reference's hitPosition: the point the shot met on the body. It
       must lie on the body the shooter could see (lag compensated), and the
       way from the fire position to it must be free: a client claiming a
       hit through a wall, or on a body that was not there, is refused */
    if (h.damage_type != P10_DMG_MELEE) {
        uint8_t   tid = (uint8_t) h.entity_id;
        player_t* target;
        HASH_FIND(hh, server->players, &tid, sizeof(tid), target);
        if (target != NULL) {
            vector3f_t pos;
            if (!lagcomp_rewind(server, player, target, &pos)) {
                pos = target->movement.position;
            }
            float hx = h.hit_position.x - pos.x, hy = h.hit_position.y - pos.y, hz = h.hit_position.z - pos.z;
            float reach = 3.0f + lagcomp_hit_tolerance();
            float dist  = sqrtf(hx * hx + hy * hy + hz * hz);
            if (dist > reach) {
                evt_log(player, "proto10", "hit_refused why=hit_position_far entity=%u dist=%.1f", h.entity_id, dist);
                return;
            }
            /* the impact lies on the body's surface, which may touch a
               block: the way is checked to 0.3 short of it */
            float fx = h.hit_position.x - h.fire_position.x, fy = h.hit_position.y - h.fire_position.y,
                  fz = h.hit_position.z - h.fire_position.z;
            float len = sqrtf(fx * fx + fy * fy + fz * fz);
            float k   = len > 0.3f ? (len - 0.3f) / len : 0.0f;
            if (!physics_line_clear(server, h.fire_position.x, h.fire_position.y, h.fire_position.z,
                                    h.fire_position.x + fx * k, h.fire_position.y + fy * k,
                                    h.fire_position.z + fz * k))
            {
                evt_log(player, "proto10", "hit_refused why=hit_blocked entity=%u dist=%.1f", h.entity_id, len);
                return;
            }
        }
    }
    uint8_t hit;
    if (h.damage_type == P10_DMG_MELEE) {
        hit = HIT_TYPE_MELEE;
    } else if (h.hit_type == P10_HIT_HEAD) {
        hit = HIT_TYPE_HEAD;
    } else if (h.hit_type == P10_HIT_LIMB) {
        hit = HIT_TYPE_ARMS;
    } else {
        hit = HIT_TYPE_TORSO;
    }
    evt_log(player, "proto10", "hit entity=%u type=%u", h.entity_id, hit);
    game_player_hit(server, player, (uint8_t) h.entity_id, hit);
}

int proto10_bridge_on_packet(server_t* server, player_t* player, uint8_t type, const uint8_t* data, size_t size)
{
    int ok = 1;
    switch (type) {
        case P10_PKT_GENERIC_COMMAND:
            _on_generic(server, player, data, size, &ok);
            break;
        case P10_PKT_CLIENT_ENTITY_UPDATE:
            _on_client_entity_update(server, player, data, size, &ok);
            break;
        case P10_PKT_PLAYER_ACTION:
            _on_player_action(server, player, data, size, &ok);
            break;
        case P10_PKT_HIT_ENTITY:
            _on_hit_entity(server, player, data, size, &ok);
            break;
        case P10_PKT_THROW_GRENADE:
            ok = proto10_entities_on_throw(server, player, data, size);
            break;
        default:
            break;
    }
    return ok;
}
