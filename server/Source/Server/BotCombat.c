// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Fighting: aim and fire, reloads, phases (cover, dodge, assault), the
    shotgun's wait, the spade up close, grenades, the snap shot layer and the
    perks (sentry, jetpack).
*/

#include <Server/GameApi.h>
#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <Server/Classes.h>
#include <modes/modes.h>
#include <Server/Proto10/Explosives.h>
#include <Server/Proto10/Rockets.h>
#include <Server/Profile.h>
#include <Util/Nanos.h>
#include <classes/classes.h>

/* Aim helper: build a direction to the target and push it off by an error
   that grows with distance and shrinks with skill. The shot then really
   travels where it is pointed, so a miss is an honest miss
   (_bot_shot_lands traces the same ray from the eye). */
void _bot_aim_with_error(server_t* server, player_t* bot, float tx, float ty, float tz, float skill)
{
    bot_ai_t* aimer = _bot_ai(bot->id);
    if (aimer != NULL) {
        aimer->aimed = 1;
    }
    float dx = tx - bot->movement.eye_pos.x;
    float dy = ty - bot->movement.eye_pos.y;
    float dz = tz - bot->movement.eye_pos.z;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.001f) {
        return;
    }
    dx /= len;
    dy /= len;
    dz /= len;

    /* never perfect: even the best bot shakes a little, a weak one sprays,
       and every bot gets worse with distance. Vertical aim is steadier (half
       the error), which still gives the odd head or leg shot. */
    float spread = (BOT_AIM_SPREAD_MIN + (1.0f - skill) * BOT_AIM_SPREAD_SKILL) * (1.0f + len / BOT_AIM_SPREAD_DIST) *
                   bot_difficulties[bot_difficulty_level].aim_error;
    /* the error drifts (drawn again every quarter second), it does not
       jump every think: a hand wobbles, and the body turning to the aim
       (_bot_turn) never settled on a point that moved a few degrees each
       100 ms, so it seldom fired */
    uint64_t now = get_nanos();
    if (aimer != NULL && now >= aimer->aim_err_at) {
        for (int k = 0; k < 3; k++) {
            aimer->aim_err[k] = gen_rand(&server->rand) - 0.5f;
        }
        aimer->aim_err_at = now + NANO_IN_MILLI * 250;
    }
    float a = (aimer != NULL ? aimer->aim_err[0] : gen_rand(&server->rand) - 0.5f) * spread;
    float b = (aimer != NULL ? aimer->aim_err[1] : gen_rand(&server->rand) - 0.5f) * spread;
    float c = (aimer != NULL ? aimer->aim_err[2] : gen_rand(&server->rand) - 0.5f) * spread * 0.5f;

    vector3f_t ori = {dx + b, dy + a, dz + c};
    float l2 = sqrtf(ori.x * ori.x + ori.y * ori.y + ori.z * ori.z);
    if (l2 < 0.001f) {
        return;
    }
    ori.x /= l2;
    ori.y /= l2;
    ori.z /= l2;
    physics_reorient_player(bot, &ori);
}

/* Where the bot's aim ray really crosses the target: HIT_TYPE_* or -1 for a
   miss. allow_shot() alone cannot judge this: physics_validate_hit accepts
   anything within 5 blocks (lag tolerance for humans), so a bot that always
   reported a hit landed nearly every shot however bad its aim was. */
/* the distance along the bot's aim to where it passes the target, 0 when
   the target is behind it */
static float _bot_shot_reach(player_t* bot, player_t* target)
{
    vector3f_t eye = bot->movement.eye_pos;
    vector3f_t dir = bot->movement.forward_orientation;
    vector3f_t to  = {target->movement.position.x - eye.x, target->movement.position.y - eye.y,
                      target->movement.position.z + BOT_BODY_CENTER - eye.z};
    float      t   = to.x * dir.x + to.y * dir.y + to.z * dir.z;
    return t > 0.0f ? t : 0.0f;
}

/* the aim's way to the target is clear of blocks: seeing a head over a
   wall is no reason to fire at the chest behind it */
static uint8_t _bot_ray_clear(server_t* server, player_t* bot, player_t* target, vector3f_t dir)
{
    vector3f_t eye = bot->movement.eye_pos;
    vector3f_t to  = {target->movement.position.x - eye.x, target->movement.position.y - eye.y,
                      target->movement.position.z + BOT_BODY_CENTER - eye.z};
    float      t   = to.x * dir.x + to.y * dir.y + to.z * dir.z;
    return t > 0.0f &&
           _bot_line_clear(server, eye.x, eye.y, eye.z, eye.x + dir.x * t, eye.y + dir.y * t, eye.z + dir.z * t);
}

static uint8_t _bot_shot_clear(server_t* server, player_t* bot, player_t* target)
{
    return _bot_ray_clear(server, bot, target, bot->movement.forward_orientation);
}

/* the first solid cell along dir from eye within t blocks: its distance
   (and the cell), or -1 when the way is clear. Cells are the map's own
   (floor of the point), as the clients' shot traces them */
static float _bot_ray_block(server_t* server, vector3f_t eye, vector3f_t dir, float t, int* bx, int* by, int* bz)
{
    *bx = *by = *bz = -1;
    for (float s = 0.0f; s < t; s += 0.1f) {
        int x = (int) floorf(eye.x + dir.x * s), y = (int) floorf(eye.y + dir.y * s);
        int z = (int) floorf(eye.z + dir.z * s);
        if (x >= 0 && y >= 0 && z >= 0 && x < server->s_map.map.size_x && y < server->s_map.map.size_y &&
            z < server->s_map.map.size_z && map_solid(&server->s_map.map, x, y, z))
        {
            *bx = x, *by = y, *bz = z;
            return s;
        }
    }
    return -1.0f;
}

static int _bot_shot_lands(player_t* bot, player_t* target, float* side_out, float* height_out)
{
    *side_out   = -1.0f;
    *height_out = 0.0f;
    vector3f_t eye = bot->movement.eye_pos;
    vector3f_t dir = bot->movement.forward_orientation;
    float      t   = _bot_shot_reach(bot, target);
    if (t <= 0.0f) {
        return -1;
    }
    /* the point of the ray closest to the target's body column */
    float px = eye.x + dir.x * t - target->movement.position.x;
    float py = eye.y + dir.y * t - target->movement.position.y;
    float pz = eye.z + dir.z * t - target->movement.position.z; /* z grows downwards: - = head, + = feet */
    float side   = sqrtf(px * px + py * py);
    *side_out    = side;
    *height_out  = pz;
    float bottom = target->crouching ? BOT_BODY_FEET_CROUCH : BOT_BODY_FEET;
    if (pz < BOT_BODY_TOP || pz > bottom) {
        return -1;
    }
    if (pz < BOT_BODY_NECK) {
        return side < BOT_HEAD_HALF_WIDTH ? HIT_TYPE_HEAD : -1;
    }
    if (side >= BOT_BODY_HALF_WIDTH) {
        return -1;
    }
    return pz > BOT_BODY_HIP ? HIT_TYPE_LEGS : HIT_TYPE_TORSO;
}

/* a full clip of the bot's gun */
static int _bot_clip_size(const player_t* bot)
{
    return (int) bot->default_clip;
}

/* how far each gun is worth firing: a bot with a shorter one closes in */
float _bot_weapon_range(const player_t* bot)
{
    return bot->weapon == WEAPON_SHOTGUN               ? 25.0f
           : bot->weapon == WEAPON_SMG                 ? 35.0f
           : (int) bot->weapon == GAME_WEAPON_ROCKET ? BOT_ROCKET_RANGE
           : (int) bot->weapon == GAME_WEAPON_REVOLVER ? BOT_REVOLVER_RANGE
                                                       : BOT_MAX_SHOT_RANGE;
}

/* An attacker on its way to the enemy intel (or with its squad on the
   lane) does not stop to fight an enemy farther than BOT_PUSH_FIGHT: it goes
   on and shoots on the move. It used to stand 70 blocks off Raindance's
   intel tower trading rifle shots with the guard behind its wall, ducking
   into cover between them, for the last 45 s of the match, and its squad
   waited behind it: attackers spent ~0 s within 30 blocks of the intel */
uint8_t _bot_pushes(server_t* server, const bot_ai_t* ai, player_t* bot, float d)
{
    /* a job at its goal (a Babel staircase, a base to mend) goes on past a
       far enemy: a staircase builder stood on its step shooting at one 117
       blocks off, 92 shots, and the staircase stayed at 14 cells of 206 */
    if (ai->work && d > BOT_PUSH_FIGHT && !_bot_carries(server, bot)) {
        return 1;
    }
    if (d <= BOT_PUSH_FIGHT || _bot_carries(server, bot) || ai->camper || (ai->role != GM_BOT_ATTACK && ai->role != GM_BOT_RUSH)) {
        return 0;
    }
    return (ai->goal_intent & GM_INTENT_PUSH) != 0;
}

/* reload the way a client asks for it (switching to the block tool cancels
   a running reload, so this also restarts it) */
static void _bot_reload(server_t* server, player_t* bot, const char* why)
{
    if (bot->reloading || bot->weapon_reserve == 0) {
        return;
    }
    bot->primary_fire = 0;
    game_player_reload(server, bot);
    evt_log(bot, "bot_reload", "clip=%u reserve=%u why=%s", bot->weapon_clip, bot->weapon_reserve, why);
}

/* The shot at an enemy it sees is held for a wall (some of the body shows
   over it, no aim gets through): after BOT_WALL_PEEK it does what a player
   does, a step or two aside to where the line is clear, and with no such
   spot it leaves that enemy alone for BOT_WALL_IGNORE. Bots stood for
   seconds aiming at the wall in front of a head they saw (`bot_shot_held
   why=wall`, one a second, 490 a 2Fort match) */
static void _bot_wall_held(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target)
{
    uint64_t now = get_nanos();
    if (ai->wall_since == 0 || ai->wall_target != target->id || now - ai->wall_last > _bot_secs(0.5f)) {
        ai->wall_target = target->id;
        ai->wall_since  = now;
    }
    ai->wall_last = now;
    if (now - ai->wall_since < _bot_secs(BOT_WALL_PEEK) || now < ai->peek_until || bot->airborne) {
        return;
    }
    float tx = target->movement.position.x, ty = target->movement.position.y;
    float tz = target->movement.position.z + BOT_BODY_CENTER;
    float dx = tx - bot->movement.position.x, dy = ty - bot->movement.position.y, dl = sqrtf(dx * dx + dy * dy);
    if (dl < 0.5f) {
        return;
    }
    float     px = -dy / dl, py = dx / dl; /* across the line to it */
    float     ez = bot->movement.eye_pos.z;
    int       fz = _bot_floor_cell(server, bot);
    map_store_t* map = &server->s_map.map;
    float     first = gen_rand(&server->rand) < 0.5f ? 1.0f : -1.0f;
    for (int k = 0; k < 4; k++) {
        float side = (k % 2 == 0) ? first : -first, step = k < 2 ? 1.0f : 2.0f;
        uint8_t ok = 1;
        /* on the same floor all the way, with room for the body */
        for (float s = 0.5f; s <= step && ok; s += 0.5f) {
            int cx = (int) floorf(bot->movement.position.x + px * side * s);
            int cy = (int) floorf(bot->movement.position.y + py * side * s);
            ok     = cx >= 0 && cy >= 0 && cx < map->size_x && cy < map->size_y && fz > 2 &&
                 _nav_solid(map, cx, cy, fz) && !_nav_solid(map, cx, cy, fz - 1) && !_nav_solid(map, cx, cy, fz - 2);
        }
        float sx = bot->movement.position.x + px * side * step, sy = bot->movement.position.y + py * side * step;
        if (ok && _bot_line_clear(server, sx, sy, ez, tx, ty, tz)) {
            ai->peek_dx    = px * side;
            ai->peek_dy    = py * side;
            ai->peek_until = now + _bot_secs(0.3f * step);
            ai->wall_since = now; /* another second from there before the next move */
            evt_log(bot, "bot_peek", "action=step target=%s side=%+.0f step=%.0f", target->name, side, step);
            return;
        }
    }
    ai->wall_since        = 0;
    ai->has_target        = 0;
    ai->wall_ignore_id    = target->id;
    ai->wall_ignore_until = now + _bot_secs(BOT_WALL_IGNORE);
    evt_log(bot, "bot_peek", "action=give_up target=%s", target->name);
}

/* the guns a player looks down the sights of at range: not the shotgun
   (it is fired from the hip up close) nor the rocket launcher (no sight) */
static uint8_t _bot_ads_gun(const player_t* bot)
{
    return bot->item == TOOL_GUN && (bot->weapon == WEAPON_RIFLE || bot->weapon == WEAPON_SMG ||
                                      (int) bot->weapon == GAME_WEAPON_REVOLVER);
}

/* Aim down the sights (the right button) while _bot_fire asked for it in
   the last BOT_ADS_HOLD seconds, through the player's own input path. Bots
   never pressed it: secondary_fire stayed 0 all match, and a spectator saw
   every bot fire from the hip. Only toggled with the trigger up and no
   reload running: game_player_fire_input with the trigger down could spend
   a round, and during a reload it cancels the reload (as a player's press
   does). A sprint drops it (Physics.c would slow a sprinting body that
   kept it: the aimed walk wins over the sprint there) */
void _bot_ads_tick(server_t* server, player_t* bot, bot_ai_t* ai)
{
    /* SP_BOT_ADS=0: never aimed, the bots as they were (a measurement) */
    static int off = -1;
    if (off < 0) {
        const char* env = getenv("SP_BOT_ADS");
        off             = env != NULL && env[0] == '0';
    }
    if (off) {
        return;
    }
    uint64_t now  = get_nanos();
    uint8_t  want = bot->alive && bot->state == STATE_READY && now < ai->ads_until && _bot_ads_gun(bot) &&
                   !bot->sprinting && !bot->reloading;
    if (ai->ads_on && !bot->secondary_fire) {
        /* the server dropped it (a reload, a sprint, an empty clip) */
        ai->ads_on = 0;
        evt_log(bot, "bot_aim", "action=off dist=%.1f weapon=%u why=dropped", ai->ads_dist, bot->weapon);
    }
    if (want == ai->ads_on || bot->primary_fire || bot->reloading) {
        return;
    }
    game_player_fire_input(server, bot, want ? 2 : 0);
    ai->ads_on = bot->secondary_fire;
    if (ai->ads_on == want) {
        evt_log(bot, "bot_aim", "action=%s dist=%.1f weapon=%u", want ? "on" : "off", ai->ads_dist, bot->weapon);
    }
}

void _bot_fire(server_t* server, player_t* bot, player_t* target)
{
    _bot_set_tool(server, bot, TOOL_GUN);
    if (bot->reloading || !bot->alive) {
        return;
    }
    /* a gun just taken out fires after its raise time, as a player's
       (player_gun_raised refuses sooner shots: bots wasted ~13 a match) */
    if (!profile_is_075() && bot->item_at != 0 &&
        get_nanos() - bot->item_at < (uint64_t) GAME_WEAPON_RAISE_MS * NANO_IN_MILLI)
    {
        return;
    }
    /* each gun at its own distance, and past it like a player would: a
       hard shot, taken less often, that the spread mostly misses (bots that
       never fired beyond 25 or 35 blocks walked past enemies in plain
       sight). A shotgun across the map is still no sniper */
    float range = _bot_weapon_range(bot);
    float rdx = target->movement.position.x - bot->movement.position.x;
    float rdy = target->movement.position.y - bot->movement.position.y;
    float rd2 = rdx * rdx + rdy * rdy;
    if (rd2 > range * range * BOT_FIRE_STRETCH * BOT_FIRE_STRETCH ||
        (rd2 > range * range && gen_rand(&server->rand) > 0.35f))
    {
        return;
    }
    /* no shot at a body it cannot see: the target is kept a few seconds out
       of sight on purpose (it ducked behind a wall, the bot should know where
       it went), and every caller fired at it anyway, into the wall between
       them. A player does not: a block takes several hits and the shots are
       wasted, and the bot knew where the enemy was only because it is a bot.
       Breaking through is the spade's job (_bot_close_quarters) */
    bot_ai_t*   shooter = _bot_ai(bot->id);
    const char* held    = !_bot_can_see(server, bot, target) ? "no_sight" : NULL;
    /* the bot sees some of the body but the aim at the chest meets a
       block: aim at what shows, as a player does (the head over a wall, the
       legs under a beam). It stood there aiming at the wall instead, a
       third of its shots held (`bot_shot_held why=wall`). Its hand's error
       too: a player who sees the head between two blocks of a window frame
       corrects the aim that lands on the frame, so the error is drawn once
       more (the same spread) before the shot is held */
    if (held == NULL && !_bot_shot_clear(server, bot, target)) {
        static const float part[5] = {-1.15f, 0.65f, BOT_BODY_CENTER, -1.15f, 0.65f}; /* head, legs, chest */
        float              skill   = shooter != NULL ? shooter->skill : 0.5f;
        held                       = "wall";
        for (int k = 0; k < 5 && held != NULL; k++) {
            if (k == 2 && shooter != NULL) {
                shooter->aim_err_at = 0;
            }
            float z = part[k] > 0.0f && target->crouching ? 0.2f : part[k];
            _bot_aim_with_error(server, bot, target->movement.position.x, target->movement.position.y,
                                target->movement.position.z + z, skill);
            if (_bot_shot_clear(server, bot, target)) {
                held = NULL;
            }
        }
    }
    if (held != NULL && held[0] == 'w' && shooter != NULL) {
        _bot_wall_held(server, bot, shooter, target);
    }
    if (held != NULL) {
        static uint64_t held_logged[BOT_MAX_PLAYERS];
        uint64_t        now_held = get_nanos();
        if (now_held - held_logged[bot->id] > NANO_IN_SECOND) {
            held_logged[bot->id] = now_held;
            /* the first solid cell on the shot's way: its own wall, a block
               away, or the ground between them */
            vector3f_t eye = bot->movement.eye_pos, dir = bot->movement.forward_orientation;
            int        bx, by, bz;
            float      at = _bot_ray_block(server, eye, dir, _bot_shot_reach(bot, target), &bx, &by, &bz);
            evt_log(bot, "bot_shot_held", "target=%s why=%s block=%d,%d,%d at=%.1f pitch=%.0f crouch=%u", target->name,
                    held, bx, by, bz, at, asinf(dir.z) * 57.3f, bot->crouching);
        }
        return;
    }
    /* a far target: the sights go up, as a player's right button does
       (_bot_ads_tick presses it through game_player_fire_input, so the
       walk slows like anyone's: Physics.c) */
    if (shooter != NULL && rd2 > BOT_ADS_MIN * BOT_ADS_MIN && _bot_ads_gun(bot)) {
        shooter->ads_until = get_nanos() + _bot_secs(BOT_ADS_HOLD);
        shooter->ads_dist  = sqrtf(rd2);
    }
    /* the think aims at once, the body turns to it over the next ticks
       (_bot_turn): the shot waits until the look really got there, or the
       view jumped onto the enemy for the shot and back */
    if (shooter != NULL && shooter->look_ok) {
        vector3f_t f = bot->movement.forward_orientation, c = shooter->look_cur;
        float      fl = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z), cl = sqrtf(c.x * c.x + c.y * c.y + c.z * c.z);
        if (fl > 0.001f && cl > 0.001f && (f.x * c.x + f.y * c.y + f.z * c.z) / (fl * cl) < BOT_FIRE_ALIGN) {
            return;
        }
        /* and the look's own way is clear: the clients draw the shot along
           the look the server sends them (look_cur), not along the aim, and
           3 degrees off at 30 blocks is a block and a half. The aim's way
           was clear and the tracer went into the wall beside it, 60% of the
           shots on de_dust (`bot_shot_wall`). The look is still turning
           onto the clear aim: the shot waits for it like for the angle */
        if (!_bot_ray_clear(server, bot, target, c)) {
            return;
        }
    }
    if (bot->weapon_clip == 0) {
        /* reloading in the open is free damage: step into cover first when
           one is within reach, and let the dodge phase walk there */
        uint64_t now = get_nanos();
        float    cx, cy;
        if (shooter != NULL &&
            _bot_cover_from(server, bot, target->movement.position.x, target->movement.position.y,
                            target->movement.position.z, 7.0f, &cx, &cy))
        {
            shooter->has_cover     = 1;
            shooter->natural_cover = 1;
            shooter->in_cover      = 0;
            shooter->cover_x       = cx;
            shooter->cover_y       = cy;
            _bot_set_phase(bot, shooter, BOT_PHASE_DODGE, now + NANO_IN_SECOND * 2);
            evt_log(bot, "bot_reload", "why=cover clip=0 at=%.1f,%.1f", cx, cy);
        }
        _bot_reload(server, bot, "empty");
        return;
    }
    uint64_t now = get_nanos();
    if (!diff_is_older_then(now, &bot->timers.since_last_weapon_input, get_player_weapon_delay_nano(bot))) {
        return;
    }
    if (bot->primary_fire) {
        /* already shooting this window, do not restart the delay */
        return;
    }

    float dx = target->movement.position.x - bot->movement.position.x;
    float dy = target->movement.position.y - bot->movement.position.y;
    float dz = target->movement.position.z - bot->movement.position.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist > BOT_VIEW_DISTANCE) {
        return;
    }
    /* a class's two guns, swapped like a player's tool slot does: the
       Commando draws the revolver from mid range in (a rocket is slow, an
       enemy that close sidesteps it, and within BOT_ROCKET_MIN its blast
       comes back), the rocket again a few blocks past BOT_REVOLVER_DRAW; the
       Marksman draws it up close (BOT_SIDEARM_DRAW: its rifle fires too
       slowly there), the rifle again a few blocks past. An enemy touching
       it gets the spade first (_bot_close_quarters) */
    if (bot->alt_weapon != 255) {
        int   rocket   = (int) bot->weapon == GAME_WEAPON_ROCKET || bot->alt_weapon == GAME_WEAPON_ROCKET;
        float draw     = rocket ? BOT_REVOLVER_DRAW : BOT_SIDEARM_DRAW;
        int   revolver = (int) bot->weapon == GAME_WEAPON_REVOLVER;
        if ((!revolver && bot->alt_weapon == GAME_WEAPON_REVOLVER && dist < draw) ||
            (revolver && dist > draw + 4.0f))
        {
            classes_swap_gun(server, bot, "bot");
            return;
        }
    }

    /* the Commando's rocket, through the client's path (rockets_fire): at
       the feet, where the blast reaches the body whatever the miss, and
       never at an enemy close enough for the blast to come back */
    if (bot->weapon == GAME_WEAPON_ROCKET) {
        if (dist < BOT_ROCKET_MIN) {
            return;
        }
        /* a rocket is slow and its blast forgives: the miss is wider than
           a bullet's (0.06 x skill landed 74 of 156 kills on 2Fort) */
        float      err = 0.05f + (1.0f - (shooter != NULL ? shooter->skill : 0.5f)) * 0.12f;
        vector3f_t eye = bot->movement.eye_pos;
        vector3f_t d   = {target->movement.position.x - eye.x + (gen_rand(&server->rand) - 0.5f) * err * dist,
                          target->movement.position.y - eye.y + (gen_rand(&server->rand) - 0.5f) * err * dist,
                          target->movement.position.z + 2.0f - eye.z};
        float      l   = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (l < 0.01f) {
            return;
        }
        d.x /= l, d.y /= l, d.z /= l;
        bot->timers.since_last_weapon_input = now;
        vector3f_t muzzle = {eye.x + d.x * 0.5f, eye.y + d.y * 0.5f, eye.z + d.z * 0.5f};
        rockets_fire(server, bot, muzzle, d, "bot");
        return;
    }

    bot->timers.since_last_weapon_input = now;
    _bot_trigger(bot);
    set_default_player_pellets(bot);
    if (shooter != NULL) {
        shooter->wall_since = 0; /* a shot got through */
    }
    /* the clients draw the shot along the look the server sends them (the
       body's, look_cur), not along the think's aim: a shot drawn into a
       block (tracer and cracks in the wall) is what players see as a bot
       shooting at a wall */
    if (shooter != NULL && shooter->look_ok) {
        vector3f_t eye = bot->movement.eye_pos, c = shooter->look_cur;
        vector3f_t to  = {target->movement.position.x - eye.x, target->movement.position.y - eye.y,
                          target->movement.position.z + BOT_BODY_CENTER - eye.z};
        float      t   = to.x * c.x + to.y * c.y + to.z * c.z;
        int        bx, by, bz;
        float      at = t > 0.0f ? _bot_ray_block(server, eye, c, t, &bx, &by, &bz) : -1.0f;
        if (at >= 0.0f) {
            evt_log(bot, "bot_shot_wall", "target=%s block=%d,%d,%d at=%.1f dist=%.1f", target->name, bx, by, bz, at,
                    t);
        }
    }

    /* Route the shot through the normal hit pipeline: allow_shot validates
       the ray and the weapon delay, and applies the standard damage. Only a
       shot whose aim ray really crosses the body gets that far. */
    float side, height;
    int   lands = _bot_shot_lands(bot, target, &side, &height);
    evt_log(bot, "bot_shot", "target=%s result=%s dist=%.1f side=%.2f height=%.2f", target->name,
            lands < 0 ? "miss" : (lands == HIT_TYPE_HEAD ? "head" : (lands == HIT_TYPE_LEGS ? "legs" : "torso")), dist,
            side, height);
    if (lands >= 0) {
        game_player_hit(server, bot, target->id, (uint8_t) lands);
    }

    /* the trigger stays down until bots_update lets go (_bot_trigger) */
    if (bot->weapon_clip > 0) {
        bot->weapon_clip--;
    }
    /* a planted shooter that has fired three times from one cell is a
       target: step sideways once (_bot_defend_hold applies the keys). The
       guard/overwatch/camper kept firing from the same spot all match. */
    if (shooter != NULL && (shooter->camper || shooter->role == GM_BOT_DEFEND)) {
        shooter->camp_shots++;
        if (shooter->camp_shots >= 3) {
            shooter->camp_shots  = 0;
            shooter->shift_until = now + NANO_IN_MILLI * 700;
            shooter->shift_dir   = (gen_rand(&server->rand) < 0.5f) ? -1 : 1;
            evt_log(bot, "bot_camp", "action=shift dir=%d", shooter->shift_dir);
        }
    }
}

const char* _bot_phase_name(bot_phase_t phase)
{
    switch (phase) {
        case BOT_PHASE_PATROL: return "patrol";
        case BOT_PHASE_BUILD: return "build";
        case BOT_PHASE_DODGE: return "dodge";
        case BOT_PHASE_ASSAULT: return "assault";
    }
    return "?";
}

void _bot_set_phase(player_t* bot, bot_ai_t* ai, bot_phase_t phase, uint64_t until)
{
    if (ai->phase != phase) {
        evt_bot_phase(bot, _bot_phase_name(phase), ai->skill);
    }
    ai->phase       = phase;
    ai->phase_until = until;
}

/* Start an engagement: wall up when it is worth it, dodge in the open when it
   is not (no blocks, in water, enemy too close to bother). */

void _bot_engage(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, float hdist, uint64_t now)
{
    ai->dodge_dir       = (gen_rand(&server->rand) < 0.5f) ? 1 : -1;
    ai->next_dodge_flip = now;
    ai->next_posture    = now;
    ai->ducked          = 0;
    ai->burst_left      = 0;
    ai->next_rebuild    = now + _bot_secs(BOT_REBUILD_DELAY);

    ai->natural_cover = 0;
    ai->in_cover      = 0;
    /* what is already there first: a wall, a crate, a corner within a few
       cells (bots raised a two block wall at every fight, 140 in a match) */
    float cx, cy;
    if (!_bot_is_wet(bot) &&
        _bot_cover_from(server, bot, target->movement.position.x, target->movement.position.y,
                        target->movement.position.z, BOT_COVER_RADIUS, &cx, &cy))
    {
        ai->has_cover     = 1;
        ai->natural_cover = 1;
        ai->cover_x       = cx;
        ai->cover_y       = cy;
        evt_log(bot, "bot_cover", "action=found spot=%.1f,%.1f dist=%.1f", cx, cy, hdist);
        _bot_set_phase(bot, ai, BOT_PHASE_DODGE, 0);
        return;
    }
    /* nothing around: sometimes a wall of its own, mostly it fights */
    if (bot->blocks > 0 && !_bot_is_wet(bot) && hdist > BOT_WALL_MIN_DIST &&
        gen_rand(&server->rand) < BOT_WALL_CHANCE && _bot_plan_wall(server, bot, target, ai))
    {
        _bot_set_phase(bot, ai, BOT_PHASE_BUILD, now + _bot_secs(3.0f));
        return;
    }
    ai->has_cover = 0;
    ai->cover_x   = bot->movement.position.x;
    ai->cover_y   = bot->movement.position.y;
    _bot_set_phase(bot, ai, BOT_PHASE_DODGE, 0);
}

/* a spot within 2.5 cells, on the bot's own floor, where a wall hides its
   chest from (ai->alert_x, y, z): the nearest one */
uint8_t _bot_find_cover(server_t* server, player_t* bot, bot_ai_t* ai, float* cx, float* cy)
{
    return _bot_cover_from(server, bot, ai->alert_x, ai->alert_y, ai->alert_z, 2.5f, cx, cy);
}

/* a spot within radius cells, on the bot's own floor, where something solid
   hides its crouched chest from (sx, sy, sz); the nearest one, preferring a
   spot where it can still look over standing up */
uint8_t _bot_cover_from(server_t* server, player_t* bot, float sx, float sy, float sz, float radius, float* cx,
                               float* cy)
{
    map_store_t* map  = &server->s_map.map;
    int       fz   = _bot_floor_cell(server, bot);
    float     best = 1e9f;
    for (float r = 1.0f; r <= radius; r += 0.75f) {
        for (int k = 0; k < 12; k++) {
            float a  = (float) k * (float) M_PI / 6.0f;
            float px = bot->movement.position.x + cosf(a) * r, py = bot->movement.position.y + sinf(a) * r;
            int   x = (int) px, y = (int) py;
            if (x < 2 || y < 2 || x >= map->size_x - 2 || y >= map->size_y - 2 || fz >= map->size_z - 1 ||
                !_nav_solid(map, x, y, fz) || !_nav_room(map, x, y, fz))
            {
                continue;
            }
            /* the crouched chest (a crouched player is about a block lower) */
            float chest = (float) fz - 1.6f;
            if (physics_can_see(server, sx, sy, sz - 1.0f, x + 0.5f, y + 0.5f, chest)) {
                continue;
            }
            /* a spot it can look over from counts as nearer */
            float score = r - (physics_can_see(server, sx, sy, sz - 1.0f, x + 0.5f, y + 0.5f, (float) fz - 2.8f) ? 1.0f : 0.0f);
            if (score < best) {
                best = score;
                *cx  = x + 0.5f;
                *cy  = y + 0.5f;
            }
        }
        if (best < 1e9f) {
            return 1;
        }
    }
    return 0;
}

/* Away from a charge it put down: of the eight ways, the dry column
   BOT_FLEE_DIST off that leads farthest from it, for the fuse and a
   second more (_think_flee walks there) */
void _bot_flee_from(server_t* server, player_t* bot, bot_ai_t* ai, float cx, float cy, uint64_t now)
{
    static const int dirs[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    float ax = bot->movement.position.x - cx, ay = bot->movement.position.y - cy;
    float best = -1e9f;
    for (int k = 0; k < 8; k++) {
        float len = k % 2 ? 1.41421f : 1.0f;
        float x = bot->movement.position.x + dirs[k][0] / len * BOT_FLEE_DIST;
        float y = bot->movement.position.y + dirs[k][1] / len * BOT_FLEE_DIST;
        int   floor;
        if (x < 1 || y < 1 || x >= server->s_map.map.size_x - 1 || y >= server->s_map.map.size_y - 1 ||
            !_bot_column_usable(server, (uint16_t) x, (uint16_t) y, &floor))
        {
            continue;
        }
        /* the farthest from the charge; even, the side the bot already
           stands on */
        float score = (x - cx) * (x - cx) + (y - cy) * (y - cy) + (dirs[k][0] * ax + dirs[k][1] * ay) / len;
        if (score > best) {
            best      = score;
            ai->flee_x = x;
            ai->flee_y = y;
        }
    }
    if (best < -1e8f) {
        /* no dry column: straight away from it */
        float l    = sqrtf(ax * ax + ay * ay);
        ai->flee_x = bot->movement.position.x + (l > 0.1f ? ax / l : 1.0f) * BOT_FLEE_DIST;
        ai->flee_y = bot->movement.position.y + (l > 0.1f ? ay / l : 0.0f) * BOT_FLEE_DIST;
    }
    ai->flee_from_x = cx;
    ai->flee_from_y = cy;
    ai->flee_until  = now + _bot_secs(GAME_DYNAMITE_FUSE + 1.0f);
    evt_log(bot, "bot_perk", "action=flee from=%.1f,%.1f to=%.1f,%.1f", cx, cy, ai->flee_x, ai->flee_y);
}

/* The sentry the bot earned, placed the way a player does (perks_on_use,
   the same PlayerAction a 1.0 client sends): at its post (the guard, a
   camper), in a fight (an enemy in sight within 40 blocks) or near the
   enemy intel, on a floor block 1 to 3 cells off, toward the enemy first,
   that sentry_cell_ok accepts. Logged as bot_perk action=sentry. Bots
   earned the perks and never used them */
uint8_t _bot_sentry(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                           uint64_t now)
{
    /* the placed perks: the sentry, and the classes' landmine and dynamite */
    int perk = (bot->perk_inventory & (1u << P10_PERK_SENTRY))     ? P10_PERK_SENTRY
               : (bot->perk_inventory & (1u << P10_PERK_LANDMINE)) ? P10_PERK_LANDMINE
               : (bot->perk_inventory & (1u << P10_PERK_DYNAMITE)) ? P10_PERK_DYNAMITE
                                                                   : -1;
    if (perk < 0 || now < ai->sentry_next || bot->airborne || _bot_is_wet(bot) || _bot_carries(server, bot)) {
        return 0;
    }
    float tx = 0.0f, ty = 0.0f;
    float td = 1e9f;
    if (target != NULL) {
        tx = target->movement.position.x - bot->movement.position.x;
        ty = target->movement.position.y - bot->movement.position.y;
        td = sqrtf(tx * tx + ty * ty);
    }
    uint8_t post   = (ai->role == GM_BOT_DEFEND && ai->dwall_planned) || (ai->camper && ai->camp_set && ai->dwall_planned);
    uint8_t fight  = visible && td < 40.0f;
    uint8_t taking = (ai->goal_intent & GM_INTENT_TAKE) && ai->goal_best < 30.0f;
    /* a landmine anywhere out of a fight (its post first: a camper's,
       the guard's), dynamite at the enemy's intel (its walls) or before an
       enemy not too close (a 4 s fuse) */
    if (perk == P10_PERK_LANDMINE) {
        /* where enemies come: our intel's surroundings (a mine at our spawn
           waited all match), or the post a camper holds */
        float hx = ai->home_x - bot->movement.position.x, hy = ai->home_y - bot->movement.position.y;
        fight = 0;
        post  = 0;
        taking = !visible && ai->home_set && hx * hx + hy * hy < 5.0f * 5.0f;
    } else if (perk == P10_PERK_DYNAMITE) {
        post  = 0;
        fight = visible && td > 6.0f && td < 15.0f;
    }
    if (!post && !fight && !taking) {
        return 0;
    }
    ai->sentry_next = now + _bot_secs(2.0f);
    if (td > 1e8f) {
        float ex, ey;
        gamemode_patrol_anchor(server, bot, &ex, &ey);
        tx = ex - bot->movement.position.x;
        ty = ey - bot->movement.position.y;
    }
    static const int dirs[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    int order[8];
    for (int k = 0; k < 8; k++) {
        order[k] = k;
    }
    /* the enemy's side first */
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            float di = dirs[order[i]][0] * tx + dirs[order[i]][1] * ty;
            float dj = dirs[order[j]][0] * tx + dirs[order[j]][1] * ty;
            if (dj > di) {
                int t    = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    int cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    int fz = _bot_floor_cell(server, bot);
    for (int r = 1; r <= 3; r++) {
        for (int k = 0; k < 8; k++) {
            int x = cx + dirs[order[k]][0] * r, y = cy + dirs[order[k]][1] * r;
            for (int z = fz - 1; z <= fz + 1; z++) {
                const char* why = NULL;
                if (z < 1 || !sentry_cell_ok(server, bot, x, y, z, &why)) {
                    continue;
                }
                perks_on_use(server, bot, p10_perk_param((uint8_t) perk, (uint32_t) x, (uint32_t) y, (uint32_t) z));
                uint8_t ok = !(bot->perk_inventory & (1u << perk));
                evt_log(bot, "bot_perk", "action=%s cell=%d,%d,%d ok=%u why=%s",
                        perk == P10_PERK_SENTRY ? "sentry" : perk == P10_PERK_LANDMINE ? "landmine" : "dynamite", x, y,
                        z, ok, post ? "post" : fight ? "fight" : "objective");
                if (ok) {
                    _bot_face(bot, x + 0.5f, y + 0.5f, (float) z);
                    ai->next_think = now + NANO_IN_MILLI * 200;
                    if (perk == P10_PERK_DYNAMITE) {
                        /* out of the blast before the fuse burns down: it
                           stood by its charge and died with the enemy */
                        _bot_flee_from(server, bot, ai, x + 0.5f, y + 0.5f, now);
                    }
                    return 1;
                }
                /* refused where the cell was fine: one sentry each at a
                   time (already_have_one), tried every 2 s 300 times a match */
                ai->sentry_next = now + _bot_secs(10.0f);
                return 0;
            }
        }
    }
    return 0;
}

/* SP_BOT_JET_OFF=<letters> turns parts of the flying off for a measurement:
   p no roofs, b no brake, g the old goal flight (12 blocks) */
static uint8_t _bot_jet_off(char part)
{
    const char* env = getenv("SP_BOT_JET_OFF");
    return env != NULL && strchr(env, part) != NULL;
}

/* A roof or a ledge within BOT_PERCH_R that a flight reaches (5 to 16
   cells over the bot, open to the sky, flat enough to stand on, dry) and
   that brings it no farther from its goal: the highest, nearer first.
   With perch > 0 the goal is a place rather than a cell (king of the hill's
   cylinder: any height over the ring counts): a roof within it is the goal,
   so a bot flies there from anywhere and over a 3 block rise, that roof
   beats height, and once the bot is nearly there only that radius' roofs
   do (a roof elsewhere is no more at the goal than the ground it is on).
   1 and its column and floor */
static int _bot_perch(server_t* server, player_t* bot, float gx, float gy, int fz, int* px, int* py, int* pz,
                      float perch)
{
    map_store_t* map = &server->s_map.map;
    float     bx = bot->movement.position.x, by = bot->movement.position.y;
    float     gd = sqrtf((gx - bx) * (gx - bx) + (gy - by) * (gy - by));
    float     best = -1e9f;
    int       near_goal = perch > 0.0f && gd <= perch + 10.0f;
    /* under a roof itself it cannot take off (flights from under an
       overhang never left the ground); near its goal it goes for the goal
       (a goal flight climbs a tower there): the roofs cost Raindance's
       pickups. Not when the goal has a perch radius: there the roof over
       the goal is the goal */
    if (map_top(map, (uint16_t) bx, (uint16_t) by) < fz - 1 || (perch <= 0.0f && gd < BOT_PERCH_GOAL)) {
        return 0;
    }
    /* inside the map's play area: a roof past Busan_Port's boundary killed
       its bot by the boundary's damage */
    int ax0 = 0, ay0 = 0, ax1 = map->size_x - 1, ay1 = map->size_y - 1;
    map_meta_play_area(server, &ax0, &ay0, &ax1, &ay1);
    for (int oy = -BOT_PERCH_R; oy <= BOT_PERCH_R; oy++) {
        for (int ox = -BOT_PERCH_R; ox <= BOT_PERCH_R; ox++) {
            int x = (int) bx + ox, y = (int) by + oy;
            if (x < 2 || y < 2 || x >= map->size_x - 2 || y >= map->size_y - 2 || x < ax0 + 3 || y < ay0 + 3 ||
                x > ax1 - 3 || y > ay1 - 3)
            {
                continue;
            }
            int top  = map_top(map, (uint16_t) x, (uint16_t) y);
            int rise = fz - top;
            /* three over it is a hop with the jetpack, and the goal's roofs
               are low ones as often as not (a town's house over the ring) */
            if (rise < (perch > 0.0f ? 3 : 5) || rise > 16 || top < 4 || top >= map->size_z - 2) {
                continue;
            }
            float tx = x + 0.5f, ty = y + 0.5f;
            float tg = sqrtf((gx - tx) * (gx - tx) + (gy - ty) * (gy - ty));
            if (tg > gd + 3.0f) {
                continue; /* not back the way it came */
            }
            int flat = 0;
            for (int ny = -1; ny <= 1; ny++) {
                for (int nx = -1; nx <= 1; nx++) {
                    flat += (nx || ny) && abs(map_top(map, (uint16_t) (x + nx), (uint16_t) (y + ny)) - top) <= 1;
                }
            }
            if (flat < 3) {
                continue; /* the tip of a pole: it would fall off */
            }
            if (near_goal && tg > perch) {
                continue; /* already at the goal: a roof elsewhere is no gain */
            }
            float score = (float) rise - 0.3f * sqrtf((float) (ox * ox + oy * oy));
            if (perch > 0.0f && tg <= perch) {
                score += 12.0f; /* that roof counts as the goal itself, which height alone never does */
            }
            /* the way up must be open: from over the bot's head to over the roof */
            if (score <= best || !physics_can_see(server, bx, by, bot->movement.position.z - 2.0f, tx, ty, (float) top - 2.5f)) {
                continue;
            }
            best = score;
            *px  = x;
            *py  = y;
            *pz  = top;
        }
    }
    return best > -1e9f;
}

static void _bot_jet_end(server_t* server, player_t* bot, bot_ai_t* ai, const char* why, int fz)
{
    jetpack_set_input(server, bot, 0);
    evt_log(bot, "bot_jet", "action=stop why=%s reason=%s floor=%d target_floor=%d fuel=%.1f", why,
            ai->jet_why == BOT_JET_PERCH ? "perch" : "goal", fz, ai->jet_tfloor, jetpack_fuel(bot));
    ai->jet_on  = 0;
    ai->jet_why = 0;
}

/* Still on the roof it flew to: the landing put it there and the hold (or
   the fight from up there) keeps it. A floor or six cells off means it
   walked away, and that is no perch any more. */
uint8_t _bot_perched(server_t* server, player_t* bot, const bot_ai_t* ai, uint64_t now)
{
    if (!ai->perch_until || ai->perch_until <= now || bot->airborne) {
        return 0;
    }
    float dx = ai->jet_tx - bot->movement.position.x, dy = ai->jet_ty - bot->movement.position.y;
    return dx * dx + dy * dy <= 36.0f && abs(_bot_floor_cell(server, bot) - ai->jet_tfloor) <= 1;
}

/* The jetpack, worn the moment it is earned, flown like a player holding
   jump (thrust while below the place, then the forward push lands it):
   - to the goal 3 levels or more above it within BOT_JET_GOAL_R, when the
     fuel covers the climb, instead of a pillar or the long way round;
   - onto a roof or a ledge on its way (_bot_perch), where it stands a few
     seconds watching from above: it flew only to a goal 3+ up within 12
     and players saw a jetpack worn and never used. In ngCTF that is the Rocketeer, who wears it from every spawn.
     A mode whose goal names a perch radius (gm_bot_goal_t.perch: king of the
     hill's ring) has roofs of its own to reach and hold.
   Logged as bot_jet action=fly|land|stop reason=goal|perch (the landing with
   at_goal=0|1 when the goal has a perch radius) */
uint8_t _bot_jetpack(server_t* server, player_t* bot, bot_ai_t* ai, float gx, float gy, int gfloor,
                            uint64_t now)
{
    int   fz   = _bot_floor_cell(server, bot);
    float fuel = jetpack_fuel(bot);
    if (!jetpack_equipped(bot) || _bot_is_wet(bot) || ai->goal_climb != 0) {
        if (ai->jet_why == BOT_JET_GOAL || ai->jet_why == BOT_JET_PERCH) {
            _bot_jet_end(server, bot, ai, "cannot", fz);
        }
        return 0;
    }
    /* a flight under way */
    if (ai->jet_why == BOT_JET_GOAL || ai->jet_why == BOT_JET_PERCH) {
        float dx = ai->jet_tx - bot->movement.position.x, dy = ai->jet_ty - bot->movement.position.y;
        float hd = sqrtf(dx * dx + dy * dy);
        if (!bot->airborne && fz <= ai->jet_tfloor + 1 && hd < 6.0f) { /* on its level: the roof, or the ledge by it */
            /* a roof over the goal is the goal itself (king of the hill's
               cylinder counts any height over the ring): the bot stays up
               there as long as it holds */
            int at_goal = 0;
            if (ai->goal_perch > 0.0f) {
                float gdist = sqrtf((gx - bot->movement.position.x) * (gx - bot->movement.position.x) +
                                    (gy - bot->movement.position.y) * (gy - bot->movement.position.y));
                at_goal = ai->jet_why == BOT_JET_PERCH && gdist <= ai->goal_perch;
                evt_log(bot, "bot_jet", "action=land reason=%s cell=%.0f,%.0f,%d fuel=%.1f at_goal=%d",
                        ai->jet_why == BOT_JET_PERCH ? "perch" : "goal", ai->jet_tx, ai->jet_ty, ai->jet_tfloor, fuel,
                        at_goal);
            } else {
                evt_log(bot, "bot_jet", "action=land reason=%s cell=%.0f,%.0f,%d fuel=%.1f",
                        ai->jet_why == BOT_JET_PERCH ? "perch" : "goal", ai->jet_tx, ai->jet_ty, ai->jet_tfloor, fuel);
            }
            if (ai->jet_why == BOT_JET_PERCH) {
                /* where it really stands is the perch (a flight may land on
                   the ledge by the roof, or on the higher tree beside it):
                   an anchor an arm's length off was no perch at the next
                   think, and the bot walked off the roof it had reached */
                ai->jet_tx     = bot->movement.position.x;
                ai->jet_ty     = bot->movement.position.y;
                ai->jet_tfloor = fz;
                ai->perch_until = now + _bot_secs(at_goal ? BOT_PERCH_HOLD_GOAL : BOT_PERCH_HOLD);
            }
            _bot_jet_end(server, bot, ai, "landed", fz);
            ai->path_len = 0; /* a new way from up here */
            return 0;
        }
        if (now > ai->jet_until || (!bot->airborne && now + _bot_secs(BOT_JET_TIMEOUT - 0.6f) > ai->jet_until)) {
            /* out of time, or back on the ground short of it */
            _bot_jet_end(server, bot, ai, now > ai->jet_until ? "timeout" : "short", fz);
            ai->jet_next = now + _bot_secs(3.0f);
            return 0;
        }
        uint8_t thrust = fz > ai->jet_tfloor - 3 && fuel > 0.0f; /* three over it: clear of the edge */
        _bot_set_tool(server, bot, TOOL_GUN);
        _bot_face(bot, ai->jet_tx, ai->jet_ty, bot->movement.position.z);
        /* straight up first while the roof's edge is ahead and above: pushing
           into the wall only scrapes along it */
        bot->move_forward = hd > 0.5f && (fz <= ai->jet_tfloor + 1 || hd > 3.0f);
        bot->sprinting    = 0;
        if (!bot->airborne && thrust) {
            bot->jumping = 1;
        }
        jetpack_set_input(server, bot, thrust);
        ai->jet_on     = thrust;
        ai->next_think = now + NANO_IN_MILLI * 50;
        return 1;
    }
    /* on a roof it flew to: watch from up there a few seconds, the gun on
       the enemy's way; a fight is the think's */
    if (now < ai->perch_until && !ai->has_target && !_bot_carries(server, bot)) {
        if (!_bot_perched(server, bot, ai, now)) {
            ai->perch_until = 0; /* it walked or fell off: no roof any more */
        } else {
            _bot_set_tool(server, bot, TOOL_GUN);
            _bot_clear_inputs(bot);
            _bot_face(bot, gx, gy, bot->movement.position.z);
            /* a roof over the goal is the goal: it keeps holding it, and the
               hold yields to a target as before (it only stops re-arming, so
               the fight's think is free) */
            if (ai->goal_perch > 0.0f) {
                float ddx = gx - bot->movement.position.x, ddy = gy - bot->movement.position.y;
                if (ddx * ddx + ddy * ddy <= ai->goal_perch * ai->goal_perch) {
                    ai->perch_until = now + _bot_secs(BOT_PERCH_HOLD_GOAL);
                }
            }
            ai->next_think = now + NANO_IN_MILLI * 200;
            return 1;
        }
    }
    if (now < ai->jet_next || bot->airborne) {
        return 0;
    }
    ai->jet_next = now + _bot_secs(1.0f);
    float dx = gx - bot->movement.position.x, dy = gy - bot->movement.position.y;
    float hd = sqrtf(dx * dx + dy * dy);
    int   rise = fz - gfloor;
    /* the fuel a climb takes: max_rise is 10 blocks a second, with a margin */
    if (gfloor > 0 && rise >= 3 && hd < (_bot_jet_off('g') ? 12.0f : BOT_JET_GOAL_R) &&
        fuel >= (_bot_jet_off('g') ? 0.8f : (float) rise / 6.0f + hd / 12.0f + 0.3f)) {
        ai->jet_why = BOT_JET_GOAL;
        ai->jet_tx = gx, ai->jet_ty = gy, ai->jet_tfloor = gfloor;
    } else if (!_bot_jet_off('p') && now >= ai->perch_next && !_bot_carries(server, bot) &&
               !(ai->goal_intent & GM_INTENT_POST) && !_bot_perched(server, bot, ai, now) &&
               fuel >= server->jetpack.fuel * BOT_PERCH_FUEL &&
               (ai->role == GM_BOT_ATTACK || ai->role == GM_BOT_RUSH) && !ai->camper)
    {
        /* attackers only: the guard (a Rocketeer in ngCTF) flew off its
           post onto roofs and Raindance's Miners took our intel 9 times of
           16 before, 1 after; a post holder of any kind keeps its post. The
           roofs are how a jetpack wearer fights, not only where it is going:
           the Rocketeer takes the high ground on its way (_bot_perched) */
        ai->perch_next = now + _bot_secs(ai->goal_perch > 0.0f ? BOT_PERCH_EVERY_GOAL : BOT_PERCH_EVERY);
        int px, py, pz;
        if (!_bot_perch(server, bot, gx, gy, fz, &px, &py, &pz, ai->goal_perch)) {
            return 0;
        }
        ai->jet_why = BOT_JET_PERCH;
        ai->jet_tx = px + 0.5f, ai->jet_ty = py + 0.5f, ai->jet_tfloor = pz;
    } else {
        return 0;
    }
    ai->jet_until = now + _bot_secs(BOT_JET_TIMEOUT);
    evt_log(bot, "bot_jet", "action=fly reason=%s floor=%d target_floor=%d dist=%.1f fuel=%.1f",
            ai->jet_why == BOT_JET_PERCH ? "perch" : "goal", fz, ai->jet_tfloor,
            sqrtf((ai->jet_tx - bot->movement.position.x) * (ai->jet_tx - bot->movement.position.x) +
                  (ai->jet_ty - bot->movement.position.y) * (ai->jet_ty - bot->movement.position.y)),
            fuel);
    return _bot_jetpack(server, bot, ai, gx, gy, gfloor, now);
}

/* Every tick: a bot with the jetpack falling fast enough to be hurt on
   landing thrusts until the fall is slow (a player taps jump), off a roof
   it flew to or a ledge the plan dropped off. Logged as bot_jet
   action=brake */
void _bot_jet_brake(server_t* server, player_t* bot, bot_ai_t* ai)
{
    if (ai->jet_why == BOT_JET_BRAKE) {
        if (!bot->airborne || bot->movement.velocity.z < 0.15f || jetpack_fuel(bot) <= 0.0f || _bot_is_wet(bot)) {
            jetpack_set_input(server, bot, 0);
            ai->jet_on  = 0;
            ai->jet_why = 0;
        }
        return;
    }
    if (ai->jet_why == 0 && !_bot_jet_off('b') && jetpack_equipped(bot) && bot->airborne && !_bot_is_wet(bot) &&
        bot->movement.velocity.z > BOT_JET_BRAKE_FALL && jetpack_fuel(bot) > 0.1f)
    {
        ai->jet_why = BOT_JET_BRAKE;
        ai->jet_on  = 1;
        jetpack_set_input(server, bot, 1);
        evt_log(bot, "bot_jet", "action=brake vz=%.2f fuel=%.1f", bot->movement.velocity.z, jetpack_fuel(bot));
    }
}

/* Where a grenade thrown from the eye in direction dir would do the most
   damage to the target, run on the server's own grenade physics (bounces
   included) tick by tick for up to 3 s: the damage a blast there would
   deal (get_grenade_damage's 4096 / d^2, in sight of the target) and the
   time it takes, the fuse */
static int _bot_nade_flight(server_t* server, player_t* bot, vector3f_t dir, vector3f_t at, float* fuse, int* self)
{
    grenade_t g;
    memset(&g, 0, sizeof(g));
    g.position   = bot->movement.eye_pos;
    g.velocity.x = dir.x + bot->movement.velocity.x;
    g.velocity.y = dir.y + bot->movement.velocity.y;
    g.velocity.z = dir.z + bot->movement.velocity.z;
    float dt     = server->physics.fsynctics > 0.0f ? server->physics.fsynctics : 1.0f / 60.0f;
    /* the flight's closest point to the target first, then one sight ray
       there: a ray at every step that got closer cost up to ~20 ms a solve
       (bot_slow on 127 bot matches) */
    float      bd2 = 12.0f * 12.0f;
    float      bt  = 0.0f;
    vector3f_t bp  = g.position;
    for (float t = dt; t <= 3.0f; t += dt) {
        physics_move_grenade(server, &g, &server->physics);
        float dx = g.position.x - at.x, dy = g.position.y - at.y, dz = g.position.z - at.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bd2 && g.position.z < 62.0f) {
            bd2 = d2;
            bt  = t;
            bp  = g.position;
        }
    }
    int best = 0;
    if (bt > 0.0f) {
        vector3f_t gp  = bp;
        float      d2  = bd2;
        float      t   = bt;
        int        dmg = d2 < 1.0f ? 100 : (int) fminf(4096.0f / d2, 100.0f);
        g.position     = gp;
        if (physics_can_see(server, at.x, at.y, at.z, g.position.x, g.position.y, g.position.z)) {
            best  = dmg;
            *fuse = t;
            /* and what it does to the thrower, standing where it is: bots
               killed themselves with a quarter of their grenades */
            vector3f_t me = bot->movement.position;
            float      sx = g.position.x - me.x, sy = g.position.y - me.y, sz = g.position.z - me.z;
            float      s2 = sx * sx + sy * sy + sz * sz;
            *self = s2 < 16.0f * 16.0f &&
                            physics_can_see(server, me.x, me.y, me.z, g.position.x, g.position.y, g.position.z)
                        ? (s2 < 1.0f ? 100 : (int) fminf(4096.0f / s2, 100.0f))
                        : 0;
        }
    }
    return best;
}

/* A grenade, the way a player throws one: at an enemy that just ducked
   behind cover (seen in the last 4 s, not now), or at two enemies close
   together, 8 to 30 blocks off. The throw is solved on the server's own
   grenade physics (pitch from level to steeply up, toward the target),
   kept only if it promises BOT_NADE_DAMAGE; the bot takes the grenade out
   with game_player_set_tool, turns to the throw, and when its look is
   there throws it as a client does (game_player_throw_grenade: the fuse,
   the eye, the velocity). Never the carrier, never in the water. Logged as
   bot_nade action=aim|throw|cancel. Bots carried 3 grenades and never
   threw one */
uint8_t _bot_grenade(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                            uint64_t now)
{
    if (ai->nade_ready) {
        player_t* t;
        HASH_FIND(hh, server->players, &ai->nade_target, sizeof(ai->nade_target), t);
        if (now > ai->nade_until || bot->grenades == 0 || _bot_carries(server, bot) || t == NULL || !t->alive) {
            ai->nade_ready = 0;
            evt_log(bot, "bot_nade", "action=cancel");
            return 0;
        }
        _bot_set_tool(server, bot, TOOL_GRENADE);
        vector3f_t d = ai->nade_dir;
        _bot_face(bot, bot->movement.eye_pos.x + d.x, bot->movement.eye_pos.y + d.y, bot->movement.eye_pos.z + d.z);
        ai->aimed     = 1; /* the fast turn */
        vector3f_t c  = ai->look_cur;
        float      cl = sqrtf(c.x * c.x + c.y * c.y + c.z * c.z);
        if (bot->item == TOOL_GRENADE && cl > 0.001f && (c.x * d.x + c.y * d.y + c.z * d.z) / cl > 0.995f) {
            uint8_t before = bot->grenades;
            bot->sprinting = 0;
            game_player_throw_grenade(server,
                                      bot,
                                      ai->nade_fuse,
                                      bot->movement.eye_pos,
                                      (vector3f_t) {d.x + bot->movement.velocity.x,
                                                     d.y + bot->movement.velocity.y,
                                                     d.z + bot->movement.velocity.z});
            evt_log(bot, "bot_nade", "action=throw target=%u fuse=%.2f ok=%u left=%u", ai->nade_target,
                    ai->nade_fuse, bot->grenades < before, bot->grenades);
            ai->nade_ready = 0;
            ai->worked_at  = now; /* the gun back in a moment (_bot_snap_shot) */
        }
        bot->move_forward = bot->move_backwards = bot->move_left = bot->move_right = 0;
        ai->next_think = now + NANO_IN_MILLI * 50;
        return 1;
    }
    if (target == NULL || bot->grenades == 0 || _bot_carries(server, bot) || now < ai->nade_next || _bot_is_wet(bot) ||
        bot->airborne || ai->goal_climb)
    {
        return 0;
    }
    float tdx = target->movement.position.x - bot->movement.position.x;
    float tdy = target->movement.position.y - bot->movement.position.y;
    float td  = sqrtf(tdx * tdx + tdy * tdy);
    if (td < BOT_NADE_MIN || td > BOT_NADE_MAX) {
        return 0;
    }
    /* behind cover, or with company */
    uint8_t why = 0;
    if (!visible && now - ai->last_seen < _bot_secs(4.0f)) {
        why = 1;
    } else if (visible) {
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (p != target && p->alive && p->team == target->team && p->state == STATE_READY) {
                float ax = p->movement.position.x - target->movement.position.x;
                float ay = p->movement.position.y - target->movement.position.y;
                if (ax * ax + ay * ay < 4.0f * 4.0f) {
                    why = 2;
                    break;
                }
            }
        }
    }
    if (!why) {
        return 0;
    }
    ai->nade_next = now + _bot_secs(BOT_NADE_EVERY * 0.3f); /* a failed solve tries again soon */
    vector3f_t at   = target->movement.position;
    float      yaw  = atan2f(tdy, tdx);
    int        best = 0;
    float      bfuse = 0.0f;
    vector3f_t bdir  = {0, 0, 0};
    for (int k = 0; k <= 14; k++) {
        float      pitch = -0.05f - 0.06f * (float) k; /* z grows downwards: negative is up */
        vector3f_t dir   = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float      fuse  = 0.0f;
        int        self  = 0;
        int        dmg   = _bot_nade_flight(server, bot, dir, at, &fuse, &self);
        if (dmg > best && self <= BOT_NADE_SELF) {
            best  = dmg;
            bfuse = fuse;
            bdir  = dir;
        }
    }
    if (best < BOT_NADE_DAMAGE) {
        return 0;
    }
    /* thrown by a hand: the weaker the bot, the wider it goes */
    float err  = (1.0f - ai->skill) * 0.08f;
    float ryaw = atan2f(bdir.y, bdir.x) + (gen_rand(&server->rand) - 0.5f) * 2.0f * err;
    float rpit = asinf(fmaxf(-1.0f, fminf(1.0f, bdir.z))) + (gen_rand(&server->rand) - 0.5f) * 2.0f * err;
    bdir       = (vector3f_t) {cosf(ryaw) * cosf(rpit), sinf(ryaw) * cosf(rpit), sinf(rpit)};
    ai->nade_ready  = 1;
    ai->nade_dir    = bdir;
    ai->nade_fuse   = bfuse;
    ai->nade_target = target->id;
    ai->nade_until  = now + _bot_secs(1.5f);
    ai->nade_next   = now + _bot_secs(BOT_NADE_EVERY);
    evt_log(bot, "bot_nade", "action=aim target=%u why=%s dist=%.1f damage=%d fuse=%.2f", target->id,
            why == 1 ? "cover" : "group", td, best, bfuse);
    _bot_set_tool(server, bot, TOOL_GRENADE);
    ai->next_think = now + NANO_IN_MILLI * 50;
    return 1;
}

#define BOT_SHOTGUN_WAIT 22.0f /* a shotgun bot waits hidden for enemies farther than this */

/* A shotgun is for close range: an enemy seen farther off is waited for,
   crouched (behind what there is), the gun on it and not fired; once it is
   close the bot stands, fires every time it can and side-steps between the
   shots. The guard did this too: it stood looking at enemies, crouched
   behind its wall, and never fired. Not the escort (it keeps up with its
   infiltrator), not a rusher or the carrier. Logged as bot_shotgun */
uint8_t _bot_shotgun(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                            uint64_t now)
{
    /* a Miner keeps after an enemy that went out of sight: under the ground
       to where it is, up beside it (it gave the enemy up the moment it
       ducked behind a wall, and its tunnels caught nobody by surprise) */
    uint8_t miner = classes_dig(server, bot) > 1 && ai->role != GM_BOT_DEFEND;
    if (bot->weapon != WEAPON_SHOTGUN || target == NULL || (!visible && !miner) || _bot_carries(server, bot) ||
        ai->role == GM_BOT_COVER || ai->role == GM_BOT_RUSH || _bot_is_wet(bot) || ai->goal_climb)
    {
        return 0;
    }
    float dx = target->movement.position.x - bot->movement.position.x;
    float dy = target->movement.position.y - bot->movement.position.y;
    float d  = sqrtf(dx * dx + dy * dy);
    /* a Miner on its way to its goal (an attacker, the enemy past
       BOT_PUSH_FIGHT) does not go after it: closing in on any enemy seen or
       remembered up to 128 blocks off took its one way from its goal to the
       enemy and back every few seconds (277 changes a match, 44% of its
       plans for an enemy), so it dug a few cells, turned round and walked
       back out, and two thirds of its runs of digs were under 5 cells. Nor
       does it push on in the open like the others (a shotgun reaches
       nothing past 30: its deaths doubled, Busan_Port 42 to 97 in three
       matches): it goes on to its goal under the ground while enemies are
       about (miner_threat, BotNav.c's miner_way). Once closing in, it lets
       go only 10 blocks further: at the line both ways it switched way
       every think (an enemy at 29.6 and 30.2) */
    if (miner && _bot_pushes(server, ai, bot, ai->closing_in ? d - 10.0f : d)) {
        if (now >= ai->miner_threat) {
            evt_log(bot, "bot_miner", "action=threat target=%u dist=%.1f", target->id, d);
        }
        ai->miner_threat = now + _bot_secs(BOT_MINER_THREAT_S);
        return 0;
    }
    /* the enemy's own floor: with no level (0) the plan went down to the
       map's bottom (a Miner's shallow digs cost more, BotPlanner.c's
       MINER_ROOF), a ramp to z 62 under an enemy that then moved on */
    int tfloor = _bot_floor_cell(server, target);
    if (!visible) {
        if (d < BOT_BREAK_RANGE || d > BOT_MINER_STALK_R || _bot_sneaking(ai)) {
            return 0; /* within reach the spade breaks its cover (_bot_close_quarters) */
        }
        if (now >= ai->close_in_until) {
            evt_log(bot, "bot_shotgun", "action=stalk target=%u dist=%.1f", target->id, d);
        }
        ai->close_in_until = now + _bot_secs(1.0f);
        _bot_set_tool(server, bot, TOOL_GUN);
        ai->routing_close_in = 1;
        uint8_t routed = _bot_route_to(server, bot, ai, target->movement.position.x, target->movement.position.y,
                                       tfloor, 0, 0);
        ai->routing_close_in = 0;
        if (!routed) {
            _bot_face(bot, target->movement.position.x, target->movement.position.y, bot->movement.position.z);
            bot->move_forward = 1;
        }
        return 1;
    }
    /* any enemy in sight (a guard sees them across the map: up to 60 only,
       it stood up and down behind its wall looking at them) */
    if (d > BOT_VIEW_DISTANCE || d < BOT_MELEE_REACH) {
        return 0;
    }
    /* a Miner never waits in the open for a far enemy: it closes in the
       hidden way, a tunnel or along the walls (its plan sneaking, digs
       cheap), and fights within BOT_MINER_CLOSE. A sneaking one (the
       infiltrator, the sneaky) keeps to its own way instead */
    if (miner && d > BOT_MINER_CLOSE) {
        if (_bot_sneaking(ai)) {
            return 0;
        }
        if (now >= ai->close_in_until) {
            evt_log(bot, "bot_shotgun", "action=close_in target=%u dist=%.1f", target->id, d);
        }
        ai->close_in_until = now + _bot_secs(1.0f);
        _bot_set_tool(server, bot, TOOL_GUN);
        ai->routing_close_in = 1;
        uint8_t routed = _bot_route_to(server, bot, ai, target->movement.position.x, target->movement.position.y,
                                       tfloor, 0, 0);
        ai->routing_close_in = 0;
        if (!routed) {
            _bot_face(bot, target->movement.position.x, target->movement.position.y, bot->movement.position.z);
            bot->move_forward = 1;
        }
        return 1;
    }
    _bot_set_tool(server, bot, TOOL_GUN);
    _bot_aim_with_error(server, bot, target->movement.position.x, target->movement.position.y,
                        target->movement.position.z + BOT_BODY_CENTER, ai->skill);
    uint8_t wait = !miner && d > BOT_SHOTGUN_WAIT;
    if (wait != ai->shotgun_wait) {
        ai->shotgun_wait = wait;
        evt_log(bot, "bot_shotgun", "action=%s target=%u dist=%.1f", wait ? "wait" : "fire", target->id, d);
    }
    if (wait) {
        bot->crouching = 1; /* low behind what there is, waiting for it */
        if (bot->weapon_clip < 6) {
            _bot_reload(server, bot, "shotgun_wait");
        }
    } else {
        bot->crouching = 0;
        if (now >= ai->next_dodge_flip) {
            ai->dodge_dir       = (int8_t) (gen_rand(&server->rand) < 0.5f ? 1 : -1);
            ai->next_dodge_flip = now + _bot_secs(0.3f + gen_rand(&server->rand) * 0.4f);
        }
        bot->move_right = ai->dodge_dir > 0;
        bot->move_left  = ai->dodge_dir < 0;
        /* in on it: the Miner to point blank, its pellets all land there */
        bot->move_forward = d > (miner ? 4.0f : 10.0f) && ai->role != GM_BOT_DEFEND;
        _bot_fire(server, bot, target);
    }
    ai->next_think = now + NANO_IN_MILLI * 60;
    return 1;
}

/* Up close, like a player: an enemy within spade reach gets the spade (80,
   through game_player_hit with HIT_TYPE_MELEE once the look is on it)
   when the bot is reloading, out of rounds or not holding a shotgun; an
   enemy that ducked behind a few blocks within BOT_BREAK_RANGE gets its
   cover dug away, the first solid cell on the line from the eye to its
   chest, when it is within reach. Logged as bot_melee and bot_path
   action=break_cover. The spade was only used to make a way */
uint8_t _bot_close_quarters(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                                   uint64_t now)
{
    if (target == NULL || _bot_carries(server, bot) || _bot_is_wet(bot) || ai->goal_climb) {
        return 0;
    }
    float dx = target->movement.position.x - bot->movement.position.x;
    float dy = target->movement.position.y - bot->movement.position.y;
    float dz = target->movement.position.z - bot->movement.position.z;
    float hd = sqrtf(dx * dx + dy * dy);
    /* right up against it: the spade, whatever the gun (a player does not
       back off to shoot someone it is touching) */
    if (visible && hd < BOT_MELEE_REACH && fabsf(dz) < 1.5f) {
        _bot_set_tool(server, bot, TOOL_SPADE);
        _bot_face(bot, target->movement.position.x, target->movement.position.y,
                  target->movement.position.z - 0.4f); /* the chest */
        ai->aimed = 1;
        bot->sprinting = 0;
        /* keep at arm's length, facing it; inside it (spawned on the same
           cell) no swing can be aimed: step back first */
        bot->move_forward   = hd > 1.2f;
        bot->move_backwards = hd < 0.5f;
        /* the look on the target itself: the swing is checked against where
           it looks (physics_validate_hit). Comparing forward_orientation with
           look_cur compared the look with itself, and 9 swings in 10 of a
           zombie closing in fast were refused as not aimed */
        vector3f_t c  = ai->look_cur;
        float      tx = dx, ty = dy, tz = dz - 0.4f;
        float      tl = sqrtf(tx * tx + ty * ty + tz * tz), cl = sqrtf(c.x * c.x + c.y * c.y + c.z * c.z);
        uint8_t    on = tl > 0.001f && cl > 0.001f && (tx * c.x + ty * c.y + tz * c.z) / (tl * cl) > 0.94f;
        if (bot->item == TOOL_SPADE && on && now >= ai->melee_next) {
            _bot_trigger(bot);
            uint8_t hp = target->hp;
            game_player_hit(server, bot, target->id, HIT_TYPE_MELEE);
            evt_log(bot, "bot_melee", "target=%u dist=%.1f hp=%u->%u", target->id, hd, hp, target->hp);
            ai->melee_next = now + NANO_IN_MILLI * 500;
        }
        ai->worked_at  = now;
        ai->next_think = now + NANO_IN_MILLI * 60;
        return 1;
    }
    if (visible || hd > BOT_BREAK_RANGE || now - ai->last_seen > _bot_secs(4.0f) ||
        !_bot_block_ready(server, bot, BLOCKACTION_DESTROY_ONE) ||
        (gamemode_player_rules(server, bot) & GM_RULE_NO_DESTROY))
    {
        return 0;
    }
    /* the first block between the eye and its chest */
    map_store_t*  map = &server->s_map.map;
    vector3f_t e   = bot->movement.eye_pos;
    float      tx = target->movement.position.x, ty = target->movement.position.y,
          tz = target->movement.position.z - 0.4f;
    float len = sqrtf((tx - e.x) * (tx - e.x) + (ty - e.y) * (ty - e.y) + (tz - e.z) * (tz - e.z));
    for (float t = 0.5f; t < len && t <= 4.0f; t += 0.25f) {
        int cx = (int) (e.x + (tx - e.x) * t / len), cy = (int) (e.y + (ty - e.y) * t / len);
        int cz = (int) (e.z + (tz - e.z) * t / len);
        if (cz < 0 || !map_solid(map, cx, cy, cz)) {
            continue;
        }
        if (cz >= map->size_z - 2 || !gamemode_cell_editable(server, cx, cy, cz)) {
            return 0;
        }
        _bot_set_tool(server, bot, TOOL_SPADE);
        _bot_face(bot, cx + 0.5f, cy + 0.5f, cz + 0.5f);
        ai->aimed      = 1;
        uint8_t ok     = _bot_block_action(server, bot, BLOCKACTION_DESTROY_ONE, cx, cy, cz);
        evt_log(bot, "bot_path", "action=break_cover cell=%d,%d,%d ok=%u target=%u", cx, cy, cz, ok, target->id);
        ai->next_think = now + NANO_IN_MILLI * 150;
        return 1;
    }
    return 0;
}

/* A think that left an enemy in plain sight alone (its role only fights
   so far, its gun is short, it is on its way to its post, it watches our
   intel, it is beyond the fight range): a player would still shoot, so the
   bot does, on the move. The feet keep the way the think chose, the gun
   turns to the enemy, the step is turned into the new frame (the keys are
   relative to where it looks). Not while it works with the spade or blocks,
   and not in the water. The carrier and the infiltrator only answer close
   ones. Logged per target as bot_snap. */
void _bot_snap_shot(server_t* server, player_t* bot, bot_ai_t* ai)
{
    uint64_t now = get_nanos();
    if (!bot->alive || _bot_is_wet(bot)) {
        return;
    }
    /* the work is done: the gun goes back in hand. Bots walked on with the
       block or the spade they last used, could not shoot and looked it */
    if (bot->item != TOOL_GUN && now - ai->worked_at > _bot_secs(BOT_WORK_GUN)) {
        _bot_set_tool(server, bot, TOOL_GUN);
    }
    if (ai->aimed || bot->item != TOOL_GUN) {
        return;
    }
    player_t* enemy = _bot_pick_target_ex(server, bot, ai, 1);
    /* nobody about and the clip half empty: reload, as a player does after
       a fight (bots only reloaded an empty clip, while firing) */
    if (enemy == NULL && !ai->has_target && bot->weapon_clip * 2 < _bot_clip_size(bot)) {
        _bot_reload(server, bot, "calm");
    }
    uint8_t   sound = 0;
    float     ex = 0.0f, ey = 0.0f, ez = 0.0f;
    if (enemy != NULL) {
        ex = enemy->movement.position.x;
        ey = enemy->movement.position.y;
        ez = enemy->movement.position.z;
    } else if (ai->engage_id != 0 && ai->engage_until > now && now >= ai->engage_cool) {
        player_t* gone;
        uint8_t   gid = (uint8_t) (ai->engage_id - 1);
        HASH_FIND(hh, server->players, &gid, sizeof(gid), gone);
        if (gone != NULL && gone->alive) {
            _bot_clear_inputs(bot);
            ai->path_step_at = now;
            _bot_face(bot, gone->movement.position.x, gone->movement.position.y,
                      gone->movement.position.z + BOT_BODY_CENTER);
            ai->aimed = 1; /* the fast turn */
            return;
        }
        ai->engage_id = 0;
        return;
    } else if (ai->steps_until > now && !(bot->move_forward || bot->move_backwards || bot->move_left ||
                                            bot->move_right)) {
        /* heard, not seen, standing: the look goes to the sound. On its way
           it does not: it glanced at every enemy behind a wall or in a tunnel
           and back (~110 glances in 150 s on 2Fort, bot_look cause=steps);
           a heard enemy is still taken on the moment it shows, outside the
           view cone too (_bot_pick_target) */
        sound          = 1;
        ai->look_cause = "steps";
        ex             = ai->steps_x;
        ey             = ai->steps_y;
        ez             = ai->steps_z;
    } else {
        return;
    }
    /* a shotgun waits hidden for far enemies and fights close ones
       (_bot_shotgun), whoever saw them first, the think or this layer */
    if (!sound && _bot_shotgun(server, bot, ai, enemy, 1, now)) {
        return;
    }
    float dx = ex - bot->movement.position.x;
    float dy = ey - bot->movement.position.y;
    float d  = sqrtf(dx * dx + dy * dy);
    /* only as far as its gun still fires (a rifle 128 blocks, about the
       fog: the first version turned on enemies 160 blocks away) */
    if ((!sound && d > _bot_weapon_range(bot) * BOT_FIRE_STRETCH) || (_bot_carries(server, bot) && d > BOT_SNAP_CARRIER) ||
        (_bot_sneaking(ai) && d > BOT_SNAP_SNEAK))
    {
        return;
    }
    /* a player who sees an enemy on its way stops, aims and shoots, and
       stays on it until it is gone; the carrier and the infiltrator only
       shoot back on the move */
    /* Only for an enemy within its gun's range, and not for long: a rifle
       bot stopped for enemies 107 blocks off across Babel's plain, the next
       one each time, and never moved again. A far one is shot at on the
       move; after 4 s standing it goes on for 3 s */
    /* nor on the last stretch to the enemy intel: it climbs on, shooting
       (bots stood at the foot of a tower trading shots with its guard) */
    uint8_t to_take = (ai->goal_intent & GM_INTENT_TAKE) && ai->goal_best < 25.0f;
    uint8_t stop = !sound && !_bot_carries(server, bot) && !_bot_sneaking(ai) && !to_take &&
                   d <= _bot_weapon_range(bot) && now >= ai->engage_cool && !_bot_pushes(server, ai, bot, d);
    if (stop && ai->engage_until > now && now - ai->engage_since > _bot_secs(BOT_ENGAGE_MAX)) {
        ai->engage_cool  = now + _bot_secs(BOT_ENGAGE_COOL);
        ai->engage_until = 0;
        ai->engage_id    = 0;
        stop             = 0;
        evt_log(bot, "bot_snap", "action=go_on stood=%.1f", BOT_ENGAGE_MAX);
    }
    if (stop) {
        if (ai->engage_until <= now) {
            ai->engage_since = now;
        }
        if (ai->engage_id != enemy->id + 1 || ai->engage_until <= now) {
            float reaction = 0.45f - ai->skill * 0.3f;
            ai->engage_fire_at = now + _bot_secs(reaction);
            evt_log(bot, "bot_snap", "action=stop target=%u dist=%.1f reaction=%.2f goal=%s", enemy->id, d, reaction,
                    ai->mode_goal ? ai->mode_goal : "none");
        }
        ai->engage_id    = (uint8_t) (enemy->id + 1);
        ai->engage_until = now + _bot_secs(BOT_ENGAGE_KEEP);
        ai->path_step_at = now; /* stopped to fight, not stuck on its way */
        _bot_clear_inputs(bot);
        _bot_aim_with_error(server, bot, ex, ey, ez + BOT_BODY_CENTER, ai->skill);
        if (now >= ai->engage_fire_at && gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
            _bot_fire(server, bot, enemy);
        }
        uint64_t next = now + NANO_IN_MILLI * 60;
        if (ai->next_think > next) {
            ai->next_think = next;
        }
        return;
    }
    /* the step the think chose, in the world */
    float fx = bot->movement.forward_orientation.x, fy = bot->movement.forward_orientation.y;
    float fl = sqrtf(fx * fx + fy * fy);
    float sx = bot->movement.strafe_orientation.x, sy = bot->movement.strafe_orientation.y;
    float sl = sqrtf(sx * sx + sy * sy);
    float ahead = (float) bot->move_forward - (float) bot->move_backwards;
    float side  = (float) bot->move_right - (float) bot->move_left;
    float mx = (fl > 0.01f ? fx / fl : 0.0f) * ahead + (sl > 0.01f ? sx / sl : 0.0f) * side;
    float my = (fl > 0.01f ? fy / fl : 0.0f) * ahead + (sl > 0.01f ? sy / sl : 0.0f) * side;
    float ml = sqrtf(mx * mx + my * my);

    if (sound) {
        _bot_face(bot, ex, ey, ez + BOT_BODY_CENTER);
    } else {
        _bot_aim_with_error(server, bot, ex, ey, ez + BOT_BODY_CENTER, ai->skill);
    }
    if (ml > 0.01f) {
        mx /= ml;
        my /= ml;
        float ax = bot->movement.forward_orientation.x, ay = bot->movement.forward_orientation.y;
        float al = sqrtf(ax * ax + ay * ay);
        ax /= al > 0.01f ? al : 1.0f;
        ay /= al > 0.01f ? al : 1.0f;
        float a2 = mx * ax + my * ay;
        float s2 = mx * bot->movement.strafe_orientation.x + my * bot->movement.strafe_orientation.y;
        bot->move_forward   = a2 > 0.38f;
        bot->move_backwards = a2 < -0.38f;
        bot->move_right     = s2 > 0.38f;
        bot->move_left      = s2 < -0.38f;
    }
    bot->sprinting = 0; /* no shooting while sprinting */
    if (!sound) {
        ai->look_cause = "sight";
    }
    uint16_t who   = (uint16_t) ((sound ? ai->steps_from : enemy->id) + 1 + (sound ? 256 : 0));
    if (ai->snap_logged != who) {
        ai->snap_logged = who;
        evt_log(bot, "bot_snap", "target=%u by=%s dist=%.1f goal=%s moving=%u", sound ? ai->steps_from : enemy->id,
                sound ? "steps" : "sight", d, ai->mode_goal ? ai->mode_goal : "none", ml > 0.01f);
    }
    if (!sound && gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
        _bot_fire(server, bot, enemy);
    }
    /* keep tracking it */
    uint64_t soon = now + NANO_IN_MILLI * 100;
    if (ai->next_think > soon) {
        ai->next_think = soon;
    }
}
