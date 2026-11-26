// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Bots fly helicopters and shoot at them (BotHeli.h). A bot uses the
 * helicopter the way a client does: `use` (mount_use) to board and to leave,
 * the movement keys, the look and the held jump / crouch to fly (what
 * Helicopter.c's _heli_think reads of any pilot), the HitEntity path
 * (proto10_objects_on_hit) and rockets_fire to shoot at one.
 */
#include <Server/BotHeli.h>
#include <Server/BotsInternal.h>
#include <Server/GameApi.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/MapStore.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Helicopter.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Rockets.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Nanos.h>
#include <Util/Weapon.h>
#include <classes/classes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

#define BH_DEG (3.14159265f / 180.0f)

/* how far a bot walks to a free helicopter of its team (a human waiting in
   its back, farther), and how far its goal must be for a flight to be worth
   the walk and the take-off */
#define BOT_HELI_BOARD_R      40.0f
#define BOT_HELI_BOARD_R_CALL 60.0f
#define BOT_HELI_GOAL_MIN     100.0f
/* a walk to the door that takes longer is given up (a helicopter on a roof
   of the base, a way round a river) */
#define BOT_HELI_BOARD_TIME   25.0f
/* after a flight or a given up walk: no new one before this */
#define BOT_HELI_COOLDOWN     30.0f
/* one roll a helicopter, then not again for this long */
#define BOT_HELI_ROLL_KEEP    60.0f
/* the flight: blocks over the highest ground ahead, the cruise speed (the
   hull's terminal speed at full tilt is ACCEL / DRAG, ~13), and how close to
   the goal it starts to land */
#define BOT_HELI_CLEAR        12.0f
#define BOT_HELI_CRUISE       13.0f
#define BOT_HELI_ARRIVE       20.0f
/* ground whose top is above this z cannot be flown over (the ceiling is z 3,
   the hull 4.5 high over its skids), and the way round it is looked for
   this far off the line */
#define BOT_HELI_TOO_HIGH     6
#define BOT_HELI_DETOUR       25.0f
/* blocks a second the kept height comes down once the high ground is past */
#define BOT_HELI_SETTLE       2.0f
/* it lands and gets out after this long in the air, below this share of
   its health, or with an enemy helicopter / machine gunner this close */
#define BOT_HELI_MAX_FLIGHT   90.0f
#define BOT_HELI_LOW_HEALTH   0.4f
#define BOT_HELI_THREAT_HELI  50.0f
#define BOT_HELI_THREAT_MG    60.0f
/* a landing that never touches down (an uneven roof under the hull, the
   rotor's wash over a slope) ends with the bot jumping out */
#define BOT_HELI_LAND_TIME    20.0f
/* the voice lines: one a bot a key in this, and one a team in the second */
#define BOT_HELI_VOICE_EACH   5.0f
#define BOT_HELI_VOICE_TEAM   1.5f

enum
{
    BH_NONE = 0,
    BH_BOARDING,
    BH_FLYING,
};

typedef struct
{
    uint8_t     state;
    uint32_t    entity;
    uint64_t    since;    /* the decision (boarding), the take-off (flying) */
    uint64_t    next_look;
    uint64_t    cooldown_until;
    uint8_t     airborne; /* took off (logged once) */
    uint8_t     landing;
    const char* land_why;
    uint64_t    land_at;
    float       land_x, land_y;
    float       dest_x, dest_y;
    float       from_x, from_y; /* where it boarded */
    uint64_t    goal_at;
    uint64_t    still_since;
    uint64_t    blocked_since; /* nothing but a wall on every way ahead */
    uint64_t    detour_logged;
    uint64_t    trace_at;
    float       zwant;    /* the height it keeps (map z), and when it was set */
    uint64_t    zwant_at;
    float       best_d;        /* the nearest it got to its goal, and when */
    uint64_t    progress_at;
    int         last_health;
    uint8_t     wrecked_seen;
    uint64_t    voice_at[2];
} bheli_t;

static bheli_t bh[BOT_MAX_PLAYERS];
static uint64_t team_voice_at[2];

/* the roll is the helicopter's, not each bot's: with eight bots by the pads
   at the start every helicopter went up whatever the chance. The first bot
   that may take one rolls for it, then nobody does for BOT_HELI_ROLL_KEEP */
#define BH_ROLLS 16
static struct
{
    uint32_t entity;
    uint64_t until;
} heli_rolls[BH_ROLLS];

static int _bh_rolled(uint32_t entity, uint64_t now)
{
    for (int i = 0; i < BH_ROLLS; i++) {
        if (heli_rolls[i].entity == entity && now < heli_rolls[i].until) {
            return 1;
        }
    }
    return 0;
}

static void _bh_roll_keep(uint32_t entity, uint64_t now)
{
    int slot = 0;
    for (int i = 0; i < BH_ROLLS; i++) {
        if (heli_rolls[i].entity == entity || now >= heli_rolls[i].until) {
            slot = i;
            break;
        }
        if (heli_rolls[i].until < heli_rolls[slot].until) {
            slot = i;
        }
    }
    heli_rolls[slot].entity = entity;
    heli_rolls[slot].until  = now + _bot_secs(BOT_HELI_ROLL_KEEP);
}

static uint8_t heli_enabled = 1;
static int     heli_chance  = 30;
/* the helicopters standing this tick (bot_heli_tick_begin): nothing to do
   on a map that has none, which is every 512 map */
static int     helis_now;
static int     objects_now;

void bot_heli_init(server_t* server)
{
    (void) server;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "bots") : NULL;
        toml_datum_t  d = t != NULL ? toml_bool_in(t, "heli_enabled") : (toml_datum_t) {0};
        if (d.ok) {
            heli_enabled = d.u.b ? 1 : 0;
        }
        d = t != NULL ? toml_int_in(t, "heli_chance") : (toml_datum_t) {0};
        if (d.ok) {
            heli_chance = (int) d.u.i;
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_BOT_HELI");
    if (env != NULL && *env != '\0') {
        heli_enabled = atoi(env) != 0;
    }
    env = getenv("SP_BOT_HELI_CHANCE");
    if (env != NULL && *env != '\0') {
        heli_chance = atoi(env);
    }
    if (heli_chance < 0) {
        heli_chance = 0;
    } else if (heli_chance > 100) {
        heli_chance = 100;
    }
    memset(bh, 0, sizeof bh);
    memset(team_voice_at, 0, sizeof team_voice_at);
    memset(heli_rolls, 0, sizeof heli_rolls);
    LOG_INFO("Bots: helicopters %s, chance %d%%", heli_enabled ? "on" : "off", heli_chance);
}

void bot_heli_tick_begin(server_t* server)
{
    (void) server;
    helis_now   = 0;
    objects_now = 0;
    if (profile_is_075()) {
        return;
    }
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o == NULL) {
            continue;
        }
        if (o->type == P10_ENT_HELICOPTER) {
            helis_now++;
            objects_now++;
        } else if (o->type == P10_ENT_SENTRY || o->type == P10_ENT_MG) {
            objects_now++;
        }
    }
}

/* `bot-voice <player> <key>` to the 1.0 clients of the bot's team (they play
   a recorded line at its body), with its team chat line: what a human would
   shout to the others. Rate limited a bot a key and a team */
static void _bh_voice(server_t* server, player_t* bot, int key, const char* const* lines, int n_lines)
{
    static const char* keys[2] = {"take_heli", "heli_down"};
    uint64_t           now     = get_nanos();
    bheli_t*           b       = &bh[bot->id];
    int                team    = bot->team == TEAM_B ? 1 : 0;
    if (now - b->voice_at[key] < _bot_secs(BOT_HELI_VOICE_EACH) && b->voice_at[key] != 0) {
        return;
    }
    if (now - team_voice_at[team] < _bot_secs(BOT_HELI_VOICE_TEAM) && team_voice_at[team] != 0) {
        return;
    }
    b->voice_at[key]    = now;
    team_voice_at[team] = now;
    bot_say(server, bot, 1, lines[(int) (gen_rand(&server->rand) * (float) n_lines) % n_lines]);
    char        id[8];
    const char* parts[3] = {"bot-voice", id, keys[key]};
    snprintf(id, sizeof id, "%u", (unsigned) bot->id);
    int       told = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot || p->conn == NULL || p->team != bot->team) {
            continue;
        }
        proto10_bridge_generic(server, p, 3, parts);
        told++;
    }
    evt_log(bot, "bot_voice", "key=%s team=%u told=%d", keys[key], (unsigned) bot->team, told);
}

static void _bh_say_take(server_t* server, player_t* bot)
{
    static const char* lines[] = {"I'll take the chopper!", "Getting in the helicopter!", "Taking the bird up!",
                                  "Chopper's mine, hold on!"};
    _bh_voice(server, bot, 0, lines, (int) (sizeof lines / sizeof *lines));
}

static void _bh_say_down(server_t* server, player_t* bot)
{
    static const char* lines[] = {"We lost the chopper!", "Helicopter down!", "The bird is down!"};
    _bh_voice(server, bot, 1, lines, (int) (sizeof lines / sizeof *lines));
}

static void _bh_reset(bheli_t* b, float cooldown)
{
    b->state          = BH_NONE;
    b->entity         = 0;
    b->airborne       = 0;
    b->landing        = 0;
    b->land_why       = NULL;
    b->still_since    = 0;
    b->wrecked_seen   = 0;
    b->cooldown_until = get_nanos() + _bot_secs(cooldown);
}

/* the distance from a point to the helicopter's box (Mount.c's reach test:
   the `use` is judged from it) */
static float _bh_box_distance(vector3f_t p, const p10_object_t* o)
{
    float c = cosf(o->box_yaw), s = sinf(o->box_yaw);
    float lx = p.x - o->base.x, ly = p.y - o->base.y;
    float rx = lx * c + ly * s, ry = -lx * s + ly * c;
    float hl = o->half_len > 0.0f ? o->half_len : o->half_width;
    rx       = fmaxf(-hl, fminf(hl, rx));
    ry       = fmaxf(-o->half_width, fminf(o->half_width, ry));
    float x = o->base.x + rx * c - ry * s, y = o->base.y + rx * s + ry * c;
    float z = fmaxf(o->base.z - o->height, fminf(o->base.z, p.z));
    float dx = p.x - x, dy = p.y - y, dz = p.z - z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int _bh_seat_free(server_t* server, const p10_object_t* o, int seat)
{
    return mount_occupant(server, o->id, seat) == NULL;
}

/* a human in a passenger seat of a pilotless helicopter: waiting for one */
static int _bh_human_waits(server_t* server, const p10_object_t* o)
{
    for (int s = 1; s < GAME_HELI_SEATS; s++) {
        player_t* p = mount_occupant(server, o->id, s);
        if (p != NULL && !p->is_bot) {
            return 1;
        }
    }
    return 0;
}

static int _bh_claimed(uint32_t entity, uint8_t except)
{
    for (int i = 0; i < BOT_MAX_PLAYERS; i++) {
        if (i != except && bh[i].state != BH_NONE && bh[i].entity == entity) {
            return 1;
        }
    }
    return 0;
}

/* where the pilot's door is: beside the cabin, ahead of the mast */
static void _bh_door(const p10_object_t* o, float* x, float* y)
{
    float c = cosf(o->heli.yaw * BH_DEG), s = sinf(o->heli.yaw * BH_DEG);
    float fwd = GAME_HELI_MAST_FORWARD + 1.6f, side = GAME_HELI_HALF_WIDTH + 0.7f;
    *x = o->heli.x + c * fwd - s * side;
    *y = o->heli.y + s * fwd + c * side;
}

/* the mode's goal for this bot (a plugin's first), or where it patrols */
static void _bh_mode_dest(server_t* server, player_t* bot, float* x, float* y)
{
    gm_bot_goal_t goal;
    memset(&goal, 0, sizeof goal);
    if (gamemode_bot_goal(server, bot, &goal)) {
        *x = goal.x;
        *y = goal.y;
    } else {
        gamemode_patrol_anchor(server, bot, x, y);
    }
    /* inside the map, and inside the play area where the map hurts outside
       it (boundary_damage) */
    map_store_t* m  = &server->s_map.map;
    int          x0 = 0, y0 = 0, x1 = m->size_x - 1, y1 = m->size_y - 1;
    map_meta_play_area(server, &x0, &y0, &x1, &y1);
    float margin = 12.0f;
    *x           = fmaxf((float) x0 + margin, fminf((float) x1 - margin, *x));
    *y           = fmaxf((float) y0 + margin, fminf((float) y1 - margin, *y));
}

void bot_heli_watch(server_t* server, player_t* bot)
{
    bheli_t* b = &bh[bot->id];
    if (b->state == BH_NONE) {
        return;
    }
    p10_object_t* o = proto10_objects_find(b->entity);
    if (b->state == BH_BOARDING) {
        if (!bot->alive || bot->state != STATE_READY) {
            evt_log(bot, "bot_heli", "action=cancel entity=%u why=death", b->entity);
            _bh_reset(b, 0.0f);
        }
        return;
    }
    /* flying: the helicopter went down under it (a wreck, a crash), or it is
       not in the seat any more (a map start, a death) */
    int down = o == NULL ? b->last_health <= 0 || b->wrecked_seen : (o->heli.wrecked || o->health <= 0);
    if (down) {
        evt_log(bot, "bot_heli", "action=down entity=%u flight=%.0f", b->entity,
                (double) (get_nanos() - b->since) / (double) NANO_IN_SECOND);
        _bh_say_down(server, bot);
        _bh_reset(b, BOT_HELI_COOLDOWN);
        return;
    }
    if (bot->mount_entity != b->entity || !bot->alive) {
        evt_log(bot, "bot_heli", "action=dismount entity=%u why=%s flight=%.0f", b->entity,
                bot->alive ? "unseated" : "death", (double) (get_nanos() - b->since) / (double) NANO_IN_SECOND);
        _bh_reset(b, BOT_HELI_COOLDOWN);
    }
}

void bot_heli_consider(server_t* server, player_t* bot, bot_ai_t* ai)
{
    if (!heli_enabled || helis_now == 0 || profile_is_075() || mount_mounted(bot)) {
        return;
    }
    bheli_t* b   = &bh[bot->id];
    uint64_t now = get_nanos();
    if (b->state == BH_BOARDING) {
        p10_object_t* o = proto10_objects_find(b->entity);
        const char*   why = NULL;
        if (o == NULL || !heli_boardable(o)) {
            why = "gone";
        } else if (!_bh_seat_free(server, o, 0)) {
            why = "taken";
        } else if (now - b->since > _bot_secs(BOT_HELI_BOARD_TIME)) {
            why = "slow";
        } else if (_bot_carries(server, bot)) {
            why = "carries";
        }
        if (why != NULL) {
            evt_log(bot, "bot_heli", "action=cancel entity=%u why=%s", b->entity, why);
            _bh_reset(b, BOT_HELI_COOLDOWN);
            return;
        }
        if (_bh_box_distance(bot->movement.eye_pos, o) > GAME_HELI_USE_RANGE - 0.3f) {
            return;
        }
        /* the client's `use`: the nearest helicopter of its team, its first
           free seat */
        mount_use(server, bot);
        if (!mount_in_vehicle(bot)) {
            return; /* refused (the reason is logged); tried again next tick */
        }
        if (bot->mount_seat != 0) {
            /* a human took the stick on the way: out of the back again */
            mount_use(server, bot);
            evt_log(bot, "bot_heli", "action=cancel entity=%u why=passenger_seat", b->entity);
            _bh_reset(b, BOT_HELI_COOLDOWN);
            return;
        }
        o              = proto10_objects_find(bot->mount_entity);
        b->state       = BH_FLYING;
        b->entity      = bot->mount_entity;
        b->since       = now;
        b->airborne    = 0;
        b->landing     = 0;
        b->goal_at     = 0;
        b->best_d      = 1e9f;
        b->progress_at = 0;
        b->blocked_since = 0;
        b->zwant_at      = 0;
        b->from_x        = bot->movement.position.x;
        b->from_y        = bot->movement.position.y;
        b->last_health = o != NULL ? o->health : 0;
        _bot_clear_inputs(bot);
        bot->primary_fire = 0;
        ai->has_target    = 0;
        ai->path_len      = 0;
        evt_log(bot, "bot_heli", "action=board entity=%u at=%.1f,%.1f,%.1f", b->entity, bot->movement.position.x,
                bot->movement.position.y, bot->movement.position.z);
        return;
    }
    if (b->state != BH_NONE || now < b->cooldown_until || now < b->next_look) {
        return;
    }
    /* once a second a bot, spread over the bots */
    b->next_look = now + _bot_secs(1.0f) + (uint64_t) bot->id * NANO_IN_MILLI * 7;
    if (ai->has_target || ai->camper || ai->work || _bot_carries(server, bot) ||
        (ai->role != GM_BOT_ATTACK && ai->role != GM_BOT_RUSH) ||
        (ai->goal_intent & (GM_INTENT_POST | GM_INTENT_GUARD | GM_INTENT_HOME | GM_INTENT_BRIEF)) ||
        (gamemode_player_rules(server, bot) & GM_RULE_NO_GUN) || (bot->team != TEAM_A && bot->team != TEAM_B))
    {
        return;
    }
    p10_object_t* best   = NULL;
    float         best_d = 1e9f;
    int           calls  = 0;
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o == NULL || o->type != P10_ENT_HELICOPTER || o->team != bot->team || !heli_boardable(o) ||
            !_bh_seat_free(server, o, 0) || _bh_claimed(o->id, bot->id) || _bh_rolled(o->id, now))
        {
            continue;
        }
        /* standing: not one in flight that its pilot just left */
        if (fabsf(o->heli.vx) + fabsf(o->heli.vy) + fabsf(o->heli.vz) > 1.0f) {
            continue;
        }
        int   waits = _bh_human_waits(server, o);
        float dx = o->heli.x - bot->movement.position.x, dy = o->heli.y - bot->movement.position.y;
        float d  = sqrtf(dx * dx + dy * dy);
        if (d > (waits ? BOT_HELI_BOARD_R_CALL : BOT_HELI_BOARD_R) ||
            fabsf(o->heli.z - bot->movement.position.z) > 12.0f)
        {
            continue;
        }
        if (waits && !calls) {
            calls  = 1;
            best   = o;
            best_d = d;
        } else if (!calls && d < best_d) {
            best   = o;
            best_d = d;
        }
    }
    if (best == NULL) {
        return;
    }
    float gx, gy;
    _bh_mode_dest(server, bot, &gx, &gy);
    float goal_d = hypotf(gx - bot->movement.position.x, gy - bot->movement.position.y);
    if (!calls && goal_d < BOT_HELI_GOAL_MIN) {
        return;
    }
    _bh_roll_keep(best->id, now);
    int ok = calls || gen_rand(&server->rand) * 100.0f < (float) heli_chance;
    evt_log(bot, "bot_heli", "action=roll entity=%u ok=%d chance=%d dist=%.0f goal_dist=%.0f called=%d", best->id, ok,
            calls ? 100 : heli_chance, best_d, goal_d, calls);
    if (!ok) {
        return;
    }
    b->state  = BH_BOARDING;
    b->entity = best->id;
    b->since  = now;
    b->dest_x = gx;
    b->dest_y = gy;
    evt_log(bot, "bot_heli", "action=decide entity=%u dist=%.0f goal_dist=%.0f", best->id, best_d, goal_d);
    _bh_say_take(server, bot);
}

int bot_heli_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    (void) server;
    bheli_t* b = &bh[bot->id];
    if (b->state != BH_BOARDING) {
        return 0;
    }
    p10_object_t* o = proto10_objects_find(b->entity);
    if (o == NULL) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    gm_goal_name(out, "board-heli", GM_INTENT_PUSH);
    _bh_door(o, &out->x, &out->y);
    out->role      = GM_BOT_ATTACK;
    out->exact     = 1;
    out->rush      = 1; /* past fights: a player running for the chopper */
    out->walk_only = 1;
    return 1;
}

/* the highest ground (the smallest map_top) under the hull at x, y with the
   heading yaw (degrees), and along the way ahead to (tx, ty) for `ahead`
   blocks, three columns wide each side */
static int _bh_ground(server_t* server, float x, float y, float yaw, float tx, float ty, float ahead)
{
    map_store_t* m   = &server->s_map.map;
    int          top = m->size_z;
    float        c = cosf(yaw * BH_DEG), s = sinf(yaw * BH_DEG);
    for (float u = -GAME_HELI_HALF_LENGTH; u <= GAME_HELI_HALF_LENGTH + 0.01f; u += 2.75f) {
        for (float v = -GAME_HELI_HALF_WIDTH; v <= GAME_HELI_HALF_WIDTH + 0.01f; v += GAME_HELI_HALF_WIDTH) {
            int t = map_top(m, (int) floorf(x + u * c - v * s), (int) floorf(y + u * s + v * c));
            top   = t < top ? t : top;
        }
    }
    float dx = tx - x, dy = ty - y, d = sqrtf(dx * dx + dy * dy);
    if (d > 0.5f && ahead > 0.0f) {
        dx /= d;
        dy /= d;
        float len = fminf(ahead, d + 6.0f);
        for (float k = 1.0f; k <= len; k += 1.0f) {
            for (int w = -1; w <= 1; w++) {
                float px = x + dx * k - dy * (float) w * 4.0f, py = y + dy * k + dx * (float) w * 4.0f;
                int   t  = map_top(m, (int) floorf(px), (int) floorf(py));
                top      = t < top ? t : top;
            }
        }
    }
    return top;
}

/* flat dry ground for the hull near (px, py) with the heading yaw: every
   column under it at one level (a step under a skid is a crash waiting),
   neither the water nor the sky's edge, inside the play area */
static int _bh_land_site(server_t* server, float px, float py, float yaw, float* ox, float* oy)
{
    map_store_t* m  = &server->s_map.map;
    int          x0 = 0, y0 = 0, x1 = m->size_x - 1, y1 = m->size_y - 1;
    map_meta_play_area(server, &x0, &y0, &x1, &y1);
    float c = cosf(yaw * BH_DEG), s = sinf(yaw * BH_DEG);
    for (int r = 0; r <= 30; r += 3) {
        int steps = r == 0 ? 1 : 2 * r;
        for (int k = 0; k < steps; k++) {
            float a = 2.0f * 3.14159265f * (float) k / (float) steps;
            float x = floorf(px + cosf(a) * (float) r) + 0.5f, y = floorf(py + sinf(a) * (float) r) + 0.5f;
            if (x < (float) x0 + 8.0f || y < (float) y0 + 8.0f || x > (float) x1 - 8.0f || y > (float) y1 - 8.0f) {
                continue;
            }
            int g = map_top(m, (int) x, (int) y);
            if (g <= 2 || g >= m->size_z - 2) {
                continue;
            }
            int flat = 1;
            for (float u = -GAME_HELI_HALF_LENGTH - 0.5f; u <= GAME_HELI_HALF_LENGTH + 0.6f && flat; u += 1.0f) {
                for (float v = -GAME_HELI_HALF_WIDTH - 0.5f; v <= GAME_HELI_HALF_WIDTH + 0.6f && flat; v += 1.0f) {
                    if (map_top(m, (int) floorf(x + u * c - v * s), (int) floorf(y + u * s + v * c)) != g) {
                        flat = 0;
                    }
                }
            }
            if (flat) {
                *ox = x;
                *oy = y;
                return 1;
            }
        }
    }
    *ox = px;
    *oy = py;
    return 0;
}

/* what makes a pilot land: an enemy helicopter in the air near it, or an
   enemy at a machine gun that sees it */
static const char* _bh_threat(server_t* server, player_t* bot, const p10_object_t* mine)
{
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o == NULL || o == mine || o->type != P10_ENT_HELICOPTER || o->team == bot->team || !heli_boardable(o) ||
            mount_occupant(server, o->id, 0) == NULL)
        {
            continue;
        }
        float dx = o->heli.x - mine->heli.x, dy = o->heli.y - mine->heli.y;
        if (dx * dx + dy * dy < BOT_HELI_THREAT_HELI * BOT_HELI_THREAT_HELI) {
            return "enemy_heli";
        }
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!p->alive || p->team == bot->team || p->team == TEAM_SPECTATOR || !mount_mounted(p) ||
            mount_in_vehicle(p))
        {
            continue;
        }
        float dx = p->movement.position.x - mine->heli.x, dy = p->movement.position.y - mine->heli.y;
        if (dx * dx + dy * dy < BOT_HELI_THREAT_MG * BOT_HELI_THREAT_MG &&
            _bot_line_clear(server, p->movement.eye_pos.x, p->movement.eye_pos.y, p->movement.eye_pos.z,
                            mine->heli.x, mine->heli.y, mine->heli.z - GAME_HELI_HEIGHT - 0.5f))
        {
            return "enemy_mg";
        }
    }
    return NULL;
}

/* now: the pilot's tick's own clock. A later get_nanos() here made the
   landing start after `now`, and `now - land_at` wrapped round to a timeout
   the moment it began (seen with SP_TIME_SCALE, where the clock moves within
   a tick) */
static void _bh_begin_landing(server_t* server, player_t* bot, bheli_t* b, const p10_object_t* o, uint64_t now,
                              float px, float py, const char* why)
{
    b->landing  = 1;
    b->land_why = why;
    b->land_at  = now;
    int found   = _bh_land_site(server, px, py, o->heli.yaw, &b->land_x, &b->land_y);
    evt_log(bot, "bot_heli", "action=land entity=%u why=%s site=%.1f,%.1f flat=%d health=%d flight=%.0f", o->id, why,
            b->land_x, b->land_y, found, o->health, (double) (get_nanos() - b->since) / (double) NANO_IN_SECOND);
}

static void _bh_get_out(server_t* server, player_t* bot, bheli_t* b, const p10_object_t* o, const char* why)
{
    evt_log(bot, "bot_heli", "action=dismount entity=%u why=%s at=%.1f,%.1f,%.1f health=%d flight=%.0f", o->id, why,
            o->heli.x, o->heli.y, o->heli.z, o->health, (double) (get_nanos() - b->since) / (double) NANO_IN_SECOND);
    _bot_clear_inputs(bot);
    bot->crouching = 0;
    /* the client's `use` again: out of the seat */
    mount_use(server, bot);
    _bh_reset(b, BOT_HELI_COOLDOWN);
}

int bot_heli_pilot(server_t* server, player_t* bot, bot_ai_t* ai)
{
    if (!mount_in_vehicle(bot)) {
        return 0;
    }
    bheli_t*      b = &bh[bot->id];
    p10_object_t* o = proto10_objects_find(bot->mount_entity);
    if (o == NULL) {
        return 1; /* Mount.c takes it off this tick ("gone") */
    }
    if (b->state != BH_FLYING || b->entity != o->id || bot->mount_seat != 0) {
        /* a seat it never chose to fly from (a passenger's): out again */
        evt_log(bot, "bot_heli", "action=dismount entity=%u why=not_pilot seat=%u", o->id, (unsigned) bot->mount_seat);
        _bot_clear_inputs(bot);
        bot->crouching = 0;
        mount_use(server, bot);
        b->state = BH_NONE;
        return 1;
    }
    uint64_t now = get_nanos();
    b->last_health = o->health;
    if (o->heli.wrecked || o->health <= 0) {
        b->wrecked_seen = 1;
        return 1; /* the wreck's path takes everyone off */
    }
    ai->has_target    = 0;
    bot->primary_fire = 0;
    bot->sprinting    = 0;
    bot->sneaking     = 0;

    heli_state* s      = &o->heli;
    float       flight = (float) (now - b->since) / (float) NANO_IN_SECOND;
    if (now >= b->goal_at) {
        b->goal_at = now + _bot_secs(1.0f);
        float ox = b->dest_x, oy = b->dest_y, nx, ny;
        _bh_mode_dest(server, bot, &nx, &ny);
        /* a goal back by the pad it took off from is no reason to land
           there: a squad's leader, a briefing, the team's own base came up
           as the goal of a bot that had decided on a 760 block trip, and it
           landed where it had taken off. It keeps the trip's goal then */
        if (hypotf(nx - b->from_x, ny - b->from_y) >= BOT_HELI_GOAL_MIN * 0.6f) {
            b->dest_x = nx;
            b->dest_y = ny;
        }
        if (hypotf(b->dest_x - ox, b->dest_y - oy) > 20.0f) {
            b->progress_at = 0; /* another goal (the hill moved): progress starts over */
        }
    }
    if (!b->landing) {
        const char* why    = NULL;
        float       px     = s->x, py = s->y;
        const char* threat = _bh_threat(server, bot, o);
        if (hypotf(b->dest_x - s->x, b->dest_y - s->y) < BOT_HELI_ARRIVE) {
            why = "arrived";
            px  = b->dest_x;
            py  = b->dest_y;
        } else if (flight > BOT_HELI_MAX_FLIGHT) {
            why = "time";
        } else if ((float) o->health < BOT_HELI_LOW_HEALTH * (float) (o->max_health > 0 ? o->max_health : 1)) {
            why = "health";
        } else if (threat != NULL) {
            why = threat;
        }
        if (why != NULL) {
            _bh_begin_landing(server, bot, b, o, now, px, py, why);
        }
    }

    float tx = b->landing ? b->land_x : b->dest_x, ty = b->landing ? b->land_y : b->dest_y;
    float dx = tx - s->x, dy = ty - s->y, d = sqrtf(dx * dx + dy * dy);
    float speed = sqrtf(s->vx * s->vx + s->vy * s->vy);
    /* the highest ground under the hull and on the way ahead (a hill, a
       tower, a crane): the cruise height is over it, and the hull climbs
       before it moves on when it is lower than that */
    int   under    = _bh_ground(server, s->x, s->y, s->yaw, tx, ty, 0.0f);
    float look     = 14.0f + speed * 2.5f;
    int   ahead    = _bh_ground(server, s->x, s->y, s->yaw, tx, ty, look);
    int   blocked  = 0;
    float goal_d   = d; /* to the target itself, whatever way round is taken */
    if (ahead < BOT_HELI_TOO_HIGH && d > 4.0f) {
        /* something on the way the hull cannot clear even at its ceiling
           (z 3: a tower or a crane up to the top of the world). A bot
           flew into one at full speed (`heli action=crash why=object`):
           it goes round it, the first heading off the line whose way is
           clear, or holds while it finds none */
        static const float offs[] = {30.0f, -30.0f, 60.0f, -60.0f, 90.0f, -90.0f};
        float              h0     = atan2f(dy, dx);
        blocked                   = 1;
        for (size_t k = 0; k < sizeof offs / sizeof offs[0]; k++) {
            float h  = h0 + offs[k] * BH_DEG;
            float px = s->x + cosf(h) * BOT_HELI_DETOUR, py = s->y + sinf(h) * BOT_HELI_DETOUR;
            int   a  = _bh_ground(server, s->x, s->y, s->yaw, px, py, BOT_HELI_DETOUR);
            if (a >= BOT_HELI_TOO_HIGH) {
                if (now - b->detour_logged > _bot_secs(2.0f)) {
                    b->detour_logged = now;
                    evt_log(bot, "bot_heli", "action=detour entity=%u off=%.0f top=%d", o->id, offs[k], ahead);
                }
                tx      = px;
                ty      = py;
                dx      = tx - s->x;
                dy      = ty - s->y;
                d       = sqrtf(dx * dx + dy * dy);
                ahead   = a;
                blocked = 0;
                break;
            }
        }
        if (blocked && b->blocked_since == 0) {
            b->blocked_since = now;
        }
    }
    if (!blocked) {
        b->blocked_since = 0;
    } else if (!b->landing && now - b->blocked_since > _bot_secs(6.0f)) {
        _bh_begin_landing(server, bot, b, o, now, s->x, s->y, "blocked");
    }
    /* no progress to the goal for a while (a hull caught on something):
       down and out */
    if (!b->landing) {
        if (goal_d < b->best_d - 2.0f || b->progress_at == 0) {
            b->best_d      = goal_d;
            b->progress_at = now;
        } else if (now - b->progress_at > _bot_secs(15.0f)) {
            _bh_begin_landing(server, bot, b, o, now, s->x, s->y, "stuck");
        }
    }
    float target_z = fmaxf(GAME_HELI_CEILING + 1.0f, (float) ahead - BOT_HELI_CLEAR);
    /* the height it keeps goes up at once for what is ahead and comes down
       slowly after it: the look ahead grows and shrinks with the speed, and
       the hull went up and down by 20 blocks between two hills, stopping to
       climb each time */
    float dt = b->zwant_at != 0 ? (float) (now - b->zwant_at) / (float) NANO_IN_SECOND : 0.0f;
    if (b->zwant_at != 0 && target_z > b->zwant) {
        target_z = fminf(target_z, b->zwant + BOT_HELI_SETTLE * dt);
    }
    b->zwant    = target_z;
    b->zwant_at = now;
    float height   = (float) under - s->z; /* blocks of air under the skids */
    int   descend  = b->landing && d < 4.0f && speed < 2.5f;

    /* the wanted velocity on the map: toward the target, slower near it,
       nothing while the hull is under its cruise height (it climbs first) */
    float want = fminf(b->landing ? 8.0f : BOT_HELI_CRUISE, 0.45f * d);
    if (!descend && (s->z > target_z + 3.0f || blocked)) {
        want = 0.0f;
    }
    float wvx = d > 0.5f ? dx / d * want : 0.0f, wvy = d > 0.5f ? dy / d * want : 0.0f;
    float ex = wvx - s->vx, ey = wvy - s->vy;
    float c = cosf(s->yaw * BH_DEG), sn = sinf(s->yaw * BH_DEG);
    float ef = ex * c + ey * sn, es = -ex * sn + ey * c;
    bot->move_forward   = ef > 1.0f;
    bot->move_backwards = ef < -1.0f;
    bot->move_right     = es > 1.0f;
    bot->move_left      = es < -1.0f;

    int lift = 0;
    if (descend) {
        /* down, gently: the sink rate (7) is over the safe landing (6),
           so the descent is let go only below 2.5 blocks a second near the
           ground and 5 higher up */
        lift = s->vz < (height > 4.0f ? 5.0f : 2.5f) ? -1 : 0;
        if (height <= 0.6f && fabsf(s->vz) < 0.5f) {
            if (b->still_since == 0) {
                b->still_since = now;
            } else if (now - b->still_since > _bot_secs(0.4f)) {
                _bh_get_out(server, bot, b, o, b->land_why != NULL ? b->land_why : "landed");
                return 1;
            }
        } else {
            b->still_since = 0;
        }
    } else {
        float dz = s->z - target_z; /* > 0: lower than wanted */
        lift     = dz > 1.0f ? 1 : (dz < -2.0f ? -1 : 0);
        b->still_since = 0;
    }
    if (b->landing && now - b->land_at > _bot_secs(BOT_HELI_LAND_TIME)) {
        /* never touched down: out, as low as it got */
        _bh_get_out(server, bot, b, o, "land_timeout");
        return 1;
    }
    /* SP_BOT_HELI_TRACE=1 (tests): the pilot's figures twice a second */
    static int trace = -1;
    if (trace < 0) {
        const char* env = getenv("SP_BOT_HELI_TRACE");
        trace           = env != NULL && *env != '\0' && strcmp(env, "0") != 0;
    }
    if (trace && now - b->trace_at > _bot_secs(0.5f)) {
        b->trace_at = now;
        evt_log(bot, "bot_heli_trace", "at=%.1f,%.1f,%.1f to=%.1f,%.1f d=%.1f v=%.1f vz=%.1f z_want=%.1f under=%d "
                "ahead=%d blocked=%d want=%.1f lift=%d landing=%d", s->x, s->y, s->z, tx, ty, d, speed, s->vz,
                target_z, under, ahead, blocked, want, lift, b->landing);
    }
    bot->jumping   = lift > 0;
    bot->crouching = lift < 0;
    if (!b->airborne && height > 3.0f) {
        b->airborne = 1;
        evt_log(bot, "bot_heli", "action=takeoff entity=%u at=%.1f,%.1f,%.1f dest=%.1f,%.1f", o->id, s->x, s->y,
                s->z, b->dest_x, b->dest_y);
    }

    /* the heading is the look's yaw: at the target from afar, kept as it is
       over it (a turn on the spot swings the tail) */
    float      yaw = d > 8.0f ? atan2f(dy, dx) : s->yaw * BH_DEG;
    vector3f_t ori = {cosf(yaw), sinf(yaw), 0.0f};
    physics_reorient_player(bot, &ori);
    if (ai->look_ok) {
        ai->look_cur  = ori;
        ai->look_want = ori;
    }
    return 1;
}

/* the body of an object a bullet aims at: the middle of its box */
static vector3f_t _bh_middle(const p10_object_t* o)
{
    vector3f_t m = {o->base.x, o->base.y, o->base.z - o->height * 0.5f};
    return m;
}

void bot_heli_shoot(server_t* server, player_t* bot, bot_ai_t* ai)
{
    if (objects_now == 0 || ai->aimed || mount_mounted(bot) || profile_is_075() || !bot->alive ||
        (gamemode_player_rules(server, bot) & GM_RULE_NO_GUN) || bh[bot->id].state != BH_NONE ||
        _bot_carries(server, bot))
    {
        return;
    }
    vector3f_t    eye    = bot->movement.eye_pos;
    int           rocket = (int) bot->weapon == GAME_WEAPON_ROCKET;
    float         range  = fminf(_bot_weapon_range(bot) * BOT_FIRE_STRETCH, BOT_VIEW_DISTANCE);
    p10_object_t* best   = NULL;
    float         best_d = 1e9f;
    vector3f_t    best_at;
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o == NULL || o->team == bot->team || o->health <= 0 || o->detonate) {
            continue;
        }
        if (o->type == P10_ENT_HELICOPTER) {
            /* one with someone aboard: an empty one at its pad is not worth
               a magazine (a new one stands there 10 s after a wreck, and
               bots by the enemy's pads shot every new one, all match) */
            if (!heli_boardable(o) || (mount_occupant(server, o->id, 0) == NULL &&
                                       mount_occupant(server, o->id, 1) == NULL &&
                                       mount_occupant(server, o->id, 2) == NULL))
            {
                continue;
            }
        } else if (o->type != P10_ENT_SENTRY && o->type != P10_ENT_MG) {
            continue;
        }
        vector3f_t m  = _bh_middle(o);
        float      dx = m.x - eye.x, dy = m.y - eye.y, dz = m.z - eye.z;
        float      d  = sqrtf(dx * dx + dy * dy + dz * dz);
        int        flown = o->type == P10_ENT_HELICOPTER && mount_occupant(server, o->id, 0) != NULL;
        if (d > range || d < 1.0f || (rocket && d < BOT_ROCKET_MIN))
        {
            continue;
        }
        /* the line to it stops a block short of its middle (the box is in
           the way of the rest) */
        float k = (d - 1.0f) / d;
        if (!_bot_line_clear(server, eye.x, eye.y, eye.z, eye.x + dx * k, eye.y + dy * k, eye.z + dz * k)) {
            continue;
        }
        /* a helicopter in the air first: it is the one that hurts */
        float score = flown ? d * 0.5f : d;
        if (score < best_d) {
            best_d  = score;
            best    = o;
            best_at = m;
        }
    }
    if (best == NULL) {
        return;
    }
    float d = sqrtf((best_at.x - eye.x) * (best_at.x - eye.x) + (best_at.y - eye.y) * (best_at.y - eye.y) +
                    (best_at.z - eye.z) * (best_at.z - eye.z));
    /* a rocket flies ROCKET_SPEED: aimed where the hull will be */
    if (rocket && best->linear) {
        for (int k = 0; k < 2; k++) {
            float t   = d / ROCKET_SPEED;
            best_at.x = _bh_middle(best).x + best->vel.x * t;
            best_at.y = _bh_middle(best).y + best->vel.y * t;
            best_at.z = _bh_middle(best).z + best->vel.z * t;
            d         = sqrtf((best_at.x - eye.x) * (best_at.x - eye.x) + (best_at.y - eye.y) * (best_at.y - eye.y) +
                              (best_at.z - eye.z) * (best_at.z - eye.z));
        }
    }
    _bot_set_tool(server, bot, TOOL_GUN);
    _bot_aim_with_error(server, bot, best_at.x, best_at.y, best_at.z, ai->skill);
    ai->look_cause = "object";
    if (bot->reloading || bot->item != TOOL_GUN) {
        return;
    }
    if (bot->item_at != 0 && get_nanos() - bot->item_at < (uint64_t) GAME_WEAPON_RAISE_MS * NANO_IN_MILLI) {
        return;
    }
    if (bot->weapon_clip == 0) {
        if (bot->weapon_reserve > 0) {
            game_player_reload(server, bot);
            evt_log(bot, "bot_reload", "clip=0 reserve=%u why=object", bot->weapon_reserve);
        }
        return;
    }
    /* the look has to be there (the body turns over the next ticks): the
       clients draw the shot along it */
    vector3f_t f = bot->movement.forward_orientation;
    if (ai->look_ok) {
        vector3f_t cl = ai->look_cur;
        float      fl = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z), ll = sqrtf(cl.x * cl.x + cl.y * cl.y + cl.z * cl.z);
        if (fl < 0.001f || ll < 0.001f || (f.x * cl.x + f.y * cl.y + f.z * cl.z) / (fl * ll) < BOT_FIRE_ALIGN) {
            return;
        }
    }
    uint64_t now = get_nanos();
    if (!diff_is_older_then(now, &bot->timers.since_last_weapon_input, get_player_weapon_delay_nano(bot)) ||
        bot->primary_fire)
    {
        return;
    }
    const char* type = best->type == P10_ENT_HELICOPTER ? "heli" : best->type == P10_ENT_SENTRY ? "sentry" : "mg";
    if (rocket) {
        bot->timers.since_last_weapon_input = now;
        vector3f_t muzzle = {eye.x + f.x * 0.5f, eye.y + f.y * 0.5f, eye.z + f.z * 0.5f};
        int        ok     = rockets_fire(server, bot, muzzle, f, "bot");
        evt_log(bot, "bot_heli_shot", "target=%u type=%s result=%s dist=%.1f", best->id, type, ok ? "rocket" : "refused",
                d);
        return;
    }
    bot->sprinting                      = 0; /* a sprinting gun is not ready (objects_on_hit) */
    bot->timers.since_last_weapon_input = now;
    _bot_trigger(bot);
    set_default_player_pellets(bot);
    if (bot->weapon_clip > 0) {
        bot->weapon_clip--;
    }
    /* the shot goes where the gun points, the error included: an honest miss */
    float      reach = fminf(d + 8.0f, range + 8.0f);
    vector3f_t to    = {eye.x + f.x * reach, eye.y + f.y * reach, eye.z + f.z * reach};
    float      t     = 0.0f;
    int        hits  = proto10_objects_segment_hits(best, eye, to, &t);
    if (hits) {
        float      k  = t * reach;
        vector3f_t at = {eye.x + f.x * (k + 0.1f), eye.y + f.y * (k + 0.1f), eye.z + f.z * (k + 0.1f)};
        if (k > 1.0f && !_bot_line_clear(server, eye.x, eye.y, eye.z, eye.x + f.x * (k - 0.5f),
                                         eye.y + f.y * (k - 0.5f), eye.z + f.z * (k - 0.5f)))
        {
            hits = 0; /* a block took it on the way */
        } else {
            /* the HitEntity a client sends for the same shot */
            p10_hit_entity hit;
            memset(&hit, 0, sizeof hit);
            hit.entity_id     = best->id;
            hit.hit_type      = P10_HIT_TORSO;
            hit.damage_type   = P10_DMG_WEAPON1;
            hit.fire_position = (p10_vec3) {eye.x, eye.y, eye.z};
            hit.hit_position  = (p10_vec3) {at.x, at.y, at.z};
            proto10_objects_on_hit(server, bot, &hit);
        }
    }
    evt_log(bot, "bot_heli_shot", "target=%u type=%s result=%s dist=%.1f", best->id, type, hits ? "hit" : "miss", d);
}
