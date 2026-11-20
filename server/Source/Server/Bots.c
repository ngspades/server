// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Server side AI bots.

    A bot is a regular player_t living in server->players, flagged with
    is_bot and with a NULL conn. It is spawned from the map spawn
    rectangle, driven through the normal input fields (move_forward,
    move_left/right, jumping, primary_fire/weapon_clip) so the shared
    physics and the regular server tick handle it exactly like a human,
    and it is removed like a disconnecting player.

    The AI is intentionally small and cheap: every bot thinks a few times
    per second and picks the nearest enemy it can see. Within shooting range
    it raises a two block wall towards it and dodges behind it, ducking and
    standing up to fire through game_player_hit (so allow_shot / weapon
    delays still apply). With no target it walks towards the enemy base,
    steering around water whenever there is a dry way.
*/

#include <Server/GameApi.h>
#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <Server/Profile.h>
#include <Server/BotProjects.h>
#include <Server/BotForts.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Sky.h>
#include <uthash/uthash.h>

static uint32_t _count_humans(server_t* server);
static void _bots_autofill_trim(server_t* server);

static bot_ai_t bot_ai[BOT_MAX_PLAYERS];
static uint8_t  autofill_target = 0;
/* a human spawned: trim the bots back to the autofill target on the next
   bots_update. Doing it at once freed players from inside the loops that
   walk the player list (the spawning player's and the next one's). */
static uint8_t  autofill_trim_pending = 0;

const bot_difficulty_t bot_difficulties[BOT_DIFFICULTY_COUNT] = {
    {"easy", 0.05f, 0.35f, 1.5f},
    {"normal", 0.15f, 0.90f, 1.0f},
    {"hard", 0.60f, 0.95f, 0.6f},
};
uint8_t bot_difficulty_level = BOT_DIFFICULTY_NORMAL;

static float _bot_roll_skill(server_t* server)
{
    const bot_difficulty_t* d = &bot_difficulties[bot_difficulty_level];
    return d->skill_min + gen_rand(&server->rand) * (d->skill_max - d->skill_min);
}

/* one per possible player id (uint8_t): never out of range */
bot_ai_t* _bot_ai(uint8_t id)
{
    return &bot_ai[id];
}

static uint8_t _find_free_id(server_t* server)
{
    for (uint8_t id = 0; id < server->protocol.max_players; ++id) {
        player_t* player;
        HASH_FIND(hh, server->players, &id, sizeof(id), player);
        if (player == NULL) {
            return id;
        }
    }
    return 0xFF;
}

/* "Bot_" and a generated name from the words the client names players
   with (common/names), so a bot reads like a player and still says it is
   one; unique like the normal join path keeps names */
static void _bot_set_name(server_t* server, player_t* bot)
{
    for (uint8_t attempt = 0; attempt < 24; ++attempt) {
        if (attempt < 20) {
            game_name_make(bot->name, PLAYER_NAME_STRLEN + 1, "Bot_",
                           (unsigned) (gen_rand(&server->rand) * GAME_NAME_ADJECTIVES),
                           (unsigned) (gen_rand(&server->rand) * GAME_NAME_NOUNS), 15);
        } else {
            snprintf(bot->name, PLAYER_NAME_STRLEN + 1, "Bot_%u", bot->id + 1);
        }
        uint8_t   match = 0;
        player_t *other, *tmp;
        HASH_ITER(hh, server->players, other, tmp)
        {
            if (other->id != bot->id && strncmp(other->name, bot->name, PLAYER_NAME_STRLEN) == 0) {
                match = 1;
                break;
            }
        }
        if (!match) {
            return;
        }
    }
}

uint8_t bots_enabled(server_t* server)
{
    return !gamemode_has(server, GM_FEAT_NO_BOTS);
}

uint8_t bot_exists(player_t* player)
{
    return player != NULL && player->is_bot;
}

uint8_t bot_holds_post(player_t* player)
{
    /* a camper at its post goes with no squad. A sneaky attacker used to
       leave its squad too, and with 8 bots a team that left one squad of
       2: it has no seat of its own, so it fills a squad and sneaks only
       past the lane (BotNav.c) */
    return player != NULL && player->is_bot && _bot_ai(player->id)->camper;
}

uint8_t bot_watches_home(player_t* player)
{
    return player != NULL && player->is_bot && _bot_ai(player->id)->overwatch;
}

const char* bot_goal_name(player_t* player)
{
    if (player == NULL || !player->is_bot || _bot_ai(player->id)->mode_goal == NULL) {
        return "none";
    }
    return _bot_ai(player->id)->mode_goal;
}

int bot_target_id(player_t* player)
{
    if (player == NULL || !player->is_bot || !_bot_ai(player->id)->has_target) {
        return -1;
    }
    return _bot_ai(player->id)->target_id;
}

uint8_t bot_line_of_sight(server_t* server, player_t* a, player_t* b)
{
    const vector3f_t* p = &a->movement.position;
    const vector3f_t* q = &b->movement.position;
    /* the eye 0.9 over the hip (z grows downwards), the chest 0.4 over it */
    return _bot_line_clear(server, p->x, p->y, p->z - 0.9f, q->x, q->y, q->z - 0.4f);
}

uint8_t bot_is_sneaky(player_t* player)
{
    return player != NULL && player->is_bot && _bot_ai(player->id)->sneaky;
}

uint8_t bot_is_tunnelling(player_t* player)
{
    if (player == NULL || !player->is_bot) {
        return 0;
    }
    bot_ai_t* ai = _bot_ai(player->id);
    return ai != NULL && ai->tun_active;
}

/* creates bots right after startup, before any client is around: used by the
   --bots flag and by automated runs, and it bypasses the host command queue
   entirely so the tick owns the creation */
/* count > 0 adds bots balanced across teams.
   count < 0 means "test": exactly ONE enemy bot, spawned on the human's own
   spawn rectangle with a fixed high skill, so a match is just the player plus
   one bot to fight (see the --bots-test flag). */
static int startup_team_bots[2];

/* --team-bots: set before server_start, created with the --bots ones */
void bots_set_startup_teams(int team1, int team2)
{
    int most            = SERVER_MAX_PLAYERS / 2;
    startup_team_bots[0] = team1 < 0 ? 0 : (team1 > most ? most : team1);
    startup_team_bots[1] = team2 < 0 ? 0 : (team2 > most ? most : team2);
}

void server_add_startup_bots(int count)
{
    server_t* server = get_server();
    for (uint8_t team = 0; team < 2; team++) {
        for (int i = 0; i < startup_team_bots[team]; i++) {
            if (bot_create(server, team) == NULL) {
                break;
            }
        }
    }
    if (startup_team_bots[0] || startup_team_bots[1]) {
        LOG_INFO("Startup bots: %d on team 1, %d on team 2", startup_team_bots[0], startup_team_bots[1]);
    }
    uint8_t   test   = (count < 0);
    uint8_t   total  = (uint8_t) (test ? 1 : count);

    for (uint8_t i = 0; i < total; i++) {
        uint8_t team;
        if (test) {
            team = TEAM_B; /* the human lands on TEAM_A, so this one is the enemy */
        } else {
            team = (server->protocol.num_team_users[TEAM_A] <= server->protocol.num_team_users[TEAM_B]) ? TEAM_A
                                                                                                      : TEAM_B;
        }

        /* the combat test subject always builds walls */
        player_t* created = test ? bot_create_beside(server, team, 0.9f, 0) : bot_create(server, team);
        if (created != NULL && test) {
            LOG_INFO("Test mode: one enemy bot %s (#%hhu) on the player's spawn area", created->name, created->id);
        }
    }
}

/* a bot that spawns on the other team's spawn area with a fixed skill:
   --bots-test's subject and the tutorial's target, both meant to be met at
   once. A dummy stands where it spawned and never fires: a beginner's first
   shots, and the perks a tutorial grants at every spawn would arm it too */
player_t* bot_create_beside(server_t* server, uint8_t team, float skill, uint8_t dummy)
{
    player_t* created = bot_create(server, team);
    if (created != NULL) {
        created->spawn_rect_override = 1;
        bot_ai_t* ai                = _bot_ai(created->id);
        if (ai != NULL) {
            ai->skill = skill;
            ai->dummy = dummy;
        }
    }
    return created;
}

uint32_t bot_count(server_t* server)
{
    uint32_t  count = 0;
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (player->is_bot) {
            count++;
        }
    }
    return count;
}

uint8_t bot_autofill_target(server_t* server)
{
    (void) server;
    return autofill_target;
}

void bot_set_autofill(server_t* server, uint8_t players_per_server)
{
    (void) server;
    autofill_target = players_per_server;
    if (autofill_target == 0) {
        return;
    }
    while (bot_count(server) + _count_humans(server) < autofill_target) {
        if (bot_create(server, TEAM_A) == NULL) {
            break;
        }
    }
}

player_t* bot_create(server_t* server, uint8_t team)
{
    if (team != TEAM_A && team != TEAM_B) {
        return NULL;
    }
    /* a mode without bots (creative): none at start-up, none from the menu */
    if (!bots_enabled(server)) {
        return NULL;
    }
    if (server->protocol.num_players >= server->protocol.max_players) {
        return NULL;
    }
    uint8_t player_id = _find_free_id(server);
    if (player_id == 0xFF) {
        return NULL;
    }

    player_t*  bot     = spadesx_calloc(1, sizeof(player_t));
    vector3f_t empty   = {0, 0, 0};
    vector3f_t forward = {1, 0, 0};
    vector3f_t height  = {0, 0, 1};
    vector3f_t strafe  = {0, 1, 0};
    init_player(server, bot, 0, 0, empty, forward, strafe, height);

    bot->id     = player_id;
    bot->conn   = NULL; /* a bot has no network connection */
    bot->is_bot = 1;
    bot->team   = team;
    bot->hp     = 100;
    bot->blocks = 50;
    bot->grenades = 3;
    bot->weapon = WEAPON_RIFLE;
    bot->item   = TOOL_GUN;
    bot->ups    = 60;
    set_default_player_ammo(bot);
    _bot_set_name(server, bot);

    server->protocol.num_players++;
    server->protocol.num_team_users[team]++;

    HASH_ADD(hh, server->players, id, sizeof(uint8_t), bot);
    HASH_SORT(server->players, player_sort);

    bot_ai_t* ai = _bot_ai(player_id);
    if (ai != NULL) {
        memset(ai, 0, sizeof(*ai));
        ai->active      = 1;
        ai->strafe_dir  = 1;

        /* Varied difficulty, like a public server: a few rookies, a few
           veterans, most in between, inside the server difficulty's range.
           Skill drives aim error, how often the bot takes cover and how long
           it peeks. */
        ai->skill = _bot_roll_skill(server);

        ai->phase       = BOT_PHASE_PATROL;
        ai->next_think  = get_nanos();
        ai->next_strafe = get_nanos();
        ai->next_hop    = get_nanos();
        /* no objective yet: the bot is still at the origin and has not spawned,
           so one picked now pointed at (0,0), out in the sea. _bot_think picks
           it after each spawn. */
    }

    /* A bot is a player without a connection, so it walks the same join
       state machine a player did on 0.75: the server tick drives
       STATE_STARTING_MAP -> JOINING (picks a team) -> SPAWNING -> READY (see
       on_player_update), which is what spawns it and lets the 1.0 bridge
       announce it. Starting it in STATE_STARTING_MAP is the entire trick. */
    bot->state = STATE_STARTING_MAP;

    if (ai != NULL && ai->spawn_in_team_rect) {
        /* test hook: drop this bot on the human's spawn area so combat starts
           immediately (see --bots-duel) */
        bot->spawn_rect_override = 1;
    }
    return bot;
}

uint8_t bot_remove(server_t* server, uint8_t player_id)
{
    player_t* bot;
    HASH_FIND(hh, server->players, &player_id, sizeof(player_id), bot);
    if (bot == NULL || !bot->is_bot) {
        return 0;
    }

    gamemode_player_out(server, bot);
    send_player_left(server, bot);

    if (server->protocol.num_team_users[bot->team] > 0) {
        server->protocol.num_team_users[bot->team]--;
    }
    if (server->protocol.num_players > 0) {
        server->protocol.num_players--;
    }

    grenade_t* elt;
    uint32_t   counter = 0;
    DL_COUNT(bot->grenade, elt, counter);

    bot_ai_t* ai = _bot_ai(bot->id);
    if (ai != NULL) {
        memset(ai, 0, sizeof(*ai));
    }

    if (counter == 0) {
        HASH_DEL(server->players, bot);
        HASH_SORT(server->players, player_sort);
        free(bot);
    } else {
        vector3f_t empty   = {0, 0, 0};
        vector3f_t forward = {1, 0, 0};
        vector3f_t height  = {0, 0, 1};
        vector3f_t strafe  = {0, 1, 0};
        init_player(server, bot, 0, 1, empty, forward, strafe, height);
        bot->is_bot = 0;
    }
    return 1;
}

uint32_t bot_remove_all(server_t* server)
{
    uint32_t  removed = 0;
    uint8_t   ids[BOT_MAX_PLAYERS];
    uint32_t  count = 0;
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (player->is_bot && count < BOT_MAX_PLAYERS) {
            ids[count++] = player->id;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (bot_remove(server, ids[i])) {
            removed++;
        }
    }
    return removed;
}
/* the parts of bots_update summed for the log every 30 s (ns) */
static uint64_t prof_refresh, prof_think, prof_snap, prof_turn, prof_ticks, prof_thinks;
/* time under a roof (a tunnel, a covered way), sampled twice a second per
   bot and logged every 30 s by kind: infiltrator, cover, sneaky attacker,
   other attackers (players asked for bots that go the hidden way) */
static uint32_t hidden_n[4], hidden_of[4];
/* the same while walking a sneaking plan: what the role decides (the whole
   time mixes in the way to the sneaking range, ~330 blocks on Raindance) */
static uint32_t hidden_sn[4], hidden_sof[4];

void _bot_face(player_t* bot, float x, float y, float z)
{
    float dx = x - bot->movement.position.x;
    float dy = y - bot->movement.position.y;
    float dz = z - bot->movement.position.z;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.001f) {
        return;
    }
    vector3f_t ori = {dx / len, dy / len, dz / len};
    physics_reorient_player(bot, &ori);
}

void _bot_clear_inputs(player_t* bot)
{
    bot->move_forward   = 0;
    bot->move_backwards = 0;
    bot->move_left      = 0;
    bot->move_right     = 0;
    bot->sprinting      = 0;
    bot->sneaking       = 0;
    bot->jumping        = 0;
}

/* Tool changes go through the same call as a client's, so everyone sees the
   bot pull out blocks and the gun again. Block placement is refused unless
   the block tool is in hand. */
void _bot_set_tool(server_t* server, player_t* bot, uint8_t tool)
{
    bot_ai_t* worker = _bot_ai(bot->id);
    if (worker != NULL && tool != TOOL_GUN) {
        worker->worked_at = get_nanos();
    }
    if (bot->item == tool) {
        return;
    }
    game_player_set_tool(server, bot, tool);
}

/* Holds the trigger for a moment. Clients only learn another player fired
   (its shot's sound and tracer, a spade's swing and the hit on the block)
   from the trigger in the entity's input, sent once per tick: a bot that
   pressed and released it in the same call was silent and invisible.
   bots_update lets go of it */
void _bot_trigger(player_t* bot)
{
    bot_ai_t* ai = _bot_ai(bot->id);
    bot->primary_fire = 1;
    if (ai != NULL) {
        ai->fire_release_at = get_nanos() + NANO_IN_MILLI * 90;
    }
}

uint64_t _bot_secs(float s)
{
    return (uint64_t) (s * (float) NANO_IN_SECOND);
}

static int8_t was_playing = -1;

uint8_t bots_playing(void)
{
    return was_playing == 1;
}

void bot_say(server_t* server, player_t* bot, uint8_t team_only, const char* text)
{
    /* the same chat a client's line reaches: the spam and team rules are a
       player's, and so is the log line */
    char line[128];
    snprintf(line, sizeof line, "%s", text);
    evt_log(bot, "bot_chat", "team=%u text=\"%s\"", team_only, text);
    game_player_chat(server, bot, team_only ? 1 : 0, line);
}

uint8_t bots_play_alone(void)
{
    static int alone = -1;
    if (alone < 0) {
        const char* env = getenv("SP_BOTS_PLAY");
        alone           = env != NULL && *env && strcmp(env, "0") != 0;
    }
    return (uint8_t) alone;
}

/* Bots only play while a real player is in the match: an empty server should
   not run a bots-only game nobody watches. Joining does not count, the match
   starts when the human actually spawns in; waiting to respawn does count,
   or the bots would freeze every time the player dies. */
static uint8_t _human_in_game(server_t* server)
{
    /* SP_BOTS_PLAY (tests, the headless benchmark): bots play alone, no
       client needed to watch */
    if (bots_play_alone()) {
        return 1;
    }
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (!player->is_bot && (player->state == STATE_READY || player->state == STATE_WAITING_FOR_RESPAWN ||
                                (player->state == STATE_SPAWNING && player->deaths > 0)))
        {
            return 1;
        }
    }
    return 0;
}

/* A thrown enemy grenade close to a bot is a survival order: reuse the
   dynamite flee (that path is already validated). It is a small, readable
   behaviour rather than a second movement system. */
static void _bots_avoid_grenades(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    if (ai->flee_until > now) {
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || p->grenade == NULL || p->grenade->exploded) {
            continue;
        }
        float dx = p->grenade->position.x - bot->movement.position.x;
        float dy = p->grenade->position.y - bot->movement.position.y;
        float dz = p->grenade->position.z - bot->movement.position.z;
        if (dx * dx + dy * dy + dz * dz <= 7.0f * 7.0f) {
            _bot_flee_from(server, bot, ai, p->grenade->position.x, p->grenade->position.y, now);
            evt_log(bot, "bot_flee", "why=grenade from=%.1f,%.1f", p->grenade->position.x, p->grenade->position.y);
            return;
        }
    }
}

/* the bots' flashlights: on the whole night (Sky.c, sky_dark: from the
   evening), off by day, never for a player the mode keeps without one
   (the zombies). Every tick, so a bot respawns with its lamp on; the dark
   changes once a game minute at most, so nothing strobes. Their light is
   the players' light too: it shows them the world round the bots */
static void bots_lights(server_t* server)
{
    int dark = sky_dark();
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot && (p->team == TEAM_A || p->team == TEAM_B)) {
            proto10_bridge_bot_light(server, p, dark, dark ? "dark" : "day");
        }
    }
}

void bots_update(server_t* server)
{
    /* the planner's copy of the map heals any edit it missed, a few columns
       a tick (a 512 map in ~68 s): every map change already calls
       planner_cell, and 512 columns a tick (~9 s) cost ~100-140 us, half of
       what all the bots' thinks did */
    uint64_t pr0 = get_real_nanos();
    planner_refresh(server, 64);
    prof_refresh += get_real_nanos() - pr0;
    prof_ticks++;
    _bots_autofill_trim(server);
    bots_lights(server);
    bot_projects_tick(server);
    bot_forts_tick(server);
    static uint64_t sight_since;
    uint64_t        tnow = get_nanos();
    if (sight_since == 0) {
        sight_since = tnow;
    } else if (tnow - sight_since >= _bot_secs(30.0f)) {
        float secs = (float) (tnow - sight_since) / (float) NANO_IN_SECOND;
        LOG_INFO("Bot sight: %.0f checks/s, %.0f rays/s", (float) sight_asked / secs, (float) sight_rays / secs);
        if (prof_ticks > 0) {
            LOG_INFO("Bot parts (avg us a tick): planner refresh %.0f, thinks %.0f (%.1f a tick, %.0f us each), "
                     "snap %.0f, turn %.0f",
                     (double) prof_refresh / prof_ticks / 1000.0, (double) prof_think / prof_ticks / 1000.0,
                     (double) prof_thinks / prof_ticks, prof_thinks ? (double) prof_think / prof_thinks / 1000.0 : 0.0,
                     (double) prof_snap / prof_ticks / 1000.0, (double) prof_turn / prof_ticks / 1000.0);
        }
        prof_refresh = prof_think = prof_snap = prof_turn = prof_ticks = prof_thinks = 0;
        sight_asked = sight_rays = 0;
        sight_since = tnow;
        if (hidden_of[0] + hidden_of[1] + hidden_of[2] + hidden_of[3] > 0) {
            float pct[4];
            for (int k = 0; k < 4; k++) {
                pct[k] = hidden_of[k] ? 100.0f * (float) hidden_n[k] / (float) hidden_of[k] : -1.0f;
            }
            float spct[4];
            for (int k = 0; k < 4; k++) {
                spct[k] = hidden_sof[k] ? 100.0f * (float) hidden_sn[k] / (float) hidden_sof[k] : -1.0f;
            }
            LOG_INFO("Bot hidden (%% of the time under a roof): infiltrate %.0f, cover %.0f, sneaky %.0f, attack %.0f; "
                     "sneaking: infiltrate %.0f (%u), cover %.0f (%u), sneaky %.0f (%u)",
                     pct[0], pct[1], pct[2], pct[3], spct[0], hidden_sof[0], spct[1], hidden_sof[1], spct[2],
                     hidden_sof[2]);
            memset(hidden_n, 0, sizeof(hidden_n));
            memset(hidden_of, 0, sizeof(hidden_of));
            memset(hidden_sn, 0, sizeof(hidden_sn));
            memset(hidden_sof, 0, sizeof(hidden_sof));
        }
    }
    uint8_t playing = _human_in_game(server);
    /* where the players are along the way from one team's spawn to the
       other's, in ten strips, sampled every 2 s of play and logged (summed
       over the match) every 30 s: how evenly a large map is held (the
       forward posts on large maps) */
    {
        static uint64_t spread_at, spread_log;
        static uint32_t spread[10];
        if (playing && tnow - spread_at >= _bot_secs(2.0f)) {
            spread_at   = tnow;
            quad3d_t* a = &server->protocol.spawns[0];
            quad3d_t* b = &server->protocol.spawns[1];
            float     ax = (a->from.x + a->to.x) * 0.5f, ay = (a->from.y + a->to.y) * 0.5f;
            float     bx = (b->from.x + b->to.x) * 0.5f, by = (b->from.y + b->to.y) * 0.5f;
            float     dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
            player_t *p, *ptmp;
            HASH_ITER(hh, server->players, p, ptmp)
            {
                if (l2 < 1.0f || p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
                    continue;
                }
                float f = ((p->movement.position.x - ax) * dx + (p->movement.position.y - ay) * dy) / l2;
                int   k = (int) (f * 10.0f);
                spread[k < 0 ? 0 : k > 9 ? 9 : k]++;
            }
        }
        if (tnow - spread_log >= _bot_secs(30.0f)) {
            spread_log = tnow;
            uint32_t total = 0;
            for (int k = 0; k < 10; k++) {
                total += spread[k];
            }
            if (total > 0) {
                LOG_INFO("Bot spread (%% of the players, team 1's spawn to team 2's): %.0f %.0f %.0f %.0f %.0f %.0f "
                         "%.0f %.0f %.0f %.0f",
                         100.0 * spread[0] / total, 100.0 * spread[1] / total, 100.0 * spread[2] / total,
                         100.0 * spread[3] / total, 100.0 * spread[4] / total, 100.0 * spread[5] / total,
                         100.0 * spread[6] / total, 100.0 * spread[7] / total, 100.0 * spread[8] / total,
                         100.0 * spread[9] / total);
            }
        }
    }
    /* how many bots stand inside another one: nothing in the physics keeps
       players apart, so _bot_keep_space is all there is. Sampled like the
       spread above and logged with it */
    {
        static uint64_t crowd_at, crowd_log;
        static uint32_t crowd_pairs, crowd_samples;
        if (playing && tnow - crowd_at >= _bot_secs(2.0f)) {
            crowd_at = tnow;
            player_t *a, *atmp, *b, *btmp;
            HASH_ITER(hh, server->players, a, atmp)
            {
                if (!a->is_bot || a->state != STATE_READY || !a->alive) {
                    continue;
                }
                HASH_ITER(hh, server->players, b, btmp)
                {
                    if (!b->is_bot || b == a || b->id < a->id || b->state != STATE_READY || !b->alive) {
                        continue;
                    }
                    float dx = a->movement.position.x - b->movement.position.x;
                    float dy = a->movement.position.y - b->movement.position.y;
                    float dz = a->movement.position.z - b->movement.position.z;
                    if (dz < 2.0f && dz > -2.0f && dx * dx + dy * dy < 1.5f * 1.5f) {
                        crowd_pairs++;
                    }
                }
            }
            crowd_samples++;
        }
        if (tnow - crowd_log >= _bot_secs(30.0f)) {
            crowd_log = tnow;
            if (crowd_samples > 0) {
                LOG_INFO("Bot crowd (bots inside 1.5 blocks of another, per sample): %.2f",
                         (double) crowd_pairs / (double) crowd_samples);
                crowd_pairs = crowd_samples = 0;
            }
        }
    }
    /* how much of the bots' time goes in the water: sampled like the spread
       above, summed over the whole run, logged every 30 s. A map where
       bots wade a lot plays badly (they swim slowly, cannot build or dig,
       and drown where the water hurts): tools/tag_maps.py reads the last
       line */
    {
        static uint64_t water_at, water_log;
        static uint32_t water_in, water_of;
        if (playing && tnow - water_at >= _bot_secs(2.0f)) {
            water_at = tnow;
            player_t *p, *ptmp;
            HASH_ITER(hh, server->players, p, ptmp)
            {
                if (!p->is_bot || p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
                    continue;
                }
                water_of++;
                water_in += p->wade != 0;
            }
        }
        if (tnow - water_log >= _bot_secs(30.0f)) {
            water_log = tnow;
            if (water_of > 0) {
                LOG_INFO("Bot water (%% of the living bots' samples in the water, the whole run): %.1f (%u of %u)",
                         100.0 * water_in / water_of, water_in, water_of);
            }
        }
    }
    /* SP_PLAY_SECONDS (tests): the bots play that many seconds of the
       game's clock with a player in, then stand still, so a measurement
       covers the same play whatever the join took (a Babel map took 9 real
       seconds to join, the whole of a 50x match) */
    static double   play_limit = -1.0;
    static uint64_t played, played_at;
    if (play_limit < 0.0) {
        const char* env = getenv("SP_PLAY_SECONDS");
        play_limit      = env != NULL ? atof(env) : 0.0;
    }
    if (play_limit > 0.0 && playing) {
        uint64_t t = get_nanos();
        played += played_at ? t - played_at : 0;
        played_at = t;
        if ((double) played / (double) NANO_IN_SECOND >= play_limit) {
            static uint8_t told;
            if (!told) {
                told = 1;
                LOG_INFO("Bots stopped: played %.0f s (SP_PLAY_SECONDS)", play_limit);
            }
            playing = 0;
        }
    }
    if (playing != was_playing) {
        LOG_INFO("Bots %s: %s", playing ? "playing" : "paused",
                 playing ? "a player is in the match" : "no player in the match");
        was_playing = (int8_t) playing;
    }

    player_t *bot, *tmp;
    HASH_ITER(hh, server->players, bot, tmp)
    {
        if (!bot->is_bot) {
            continue;
        }
        bot_ai_t* ai = _bot_ai(bot->id);
        if (ai == NULL || !ai->active) {
            continue;
        }
        if (ai->fire_release_at != 0 && get_nanos() >= ai->fire_release_at) {
            bot->primary_fire   = 0;
            ai->fire_release_at = 0;
        }
        if (bot->state != STATE_READY && bot->team != TEAM_SPECTATOR) {
            _bot_role_weapon(server, bot, ai);
        }
        /* a death ends the Miner's tunnel being measured (the bot waits to
           respawn out of STATE_READY, before the dead branch below) */
        if (ai->tun_active && (!bot->alive || bot->state != STATE_READY)) {
            _bot_tunnel_end(bot, ai, "death", get_nanos());
        }
        if (bot->state != STATE_READY || !playing || ai->dummy) {
            bot->primary_fire = 0;
            _bot_clear_inputs(bot);
            bot->crouching = 0;
            ai->has_target = 0;
            continue;
        }
        if (!bot->alive) {
            bot->primary_fire = 0;
            _bot_clear_inputs(bot);
            ai->phase      = BOT_PHASE_PATROL;
            ai->has_target = 0;
            ai->has_cover  = 0;
            ai->escaping   = 0;
            ai->has_objective = 0;
            ai->path_len      = 0;
            ai->alert_until   = 0;
            ai->dwall_planned = 0;
            ai->camp_set      = 0;
            ai->camp_try_at   = 0; /* dead time is no try */
            ai->camp_shots    = 0;
            ai->shift_until   = 0;
            ai->shift_dir     = 0;
            ai->beat_next     = 0;
            ai->beat_on       = 0;
            ai->plan_ticket   = 0; /* a plan for the last life is of no use */
            _bot_tunnel_end(bot, ai, "death", get_nanos());
            ai->closing_in    = 0;
            ai->kept_len      = 0;
            ai->miner_threat  = 0;
            ai->peek_until    = 0;
            ai->wall_since    = 0;
            ai->look_ok       = 0; /* the next life looks where it spawns */
            /* the first think of the next life somewhere in the next 0.4 s:
               127 bots spawning in one tick all thought in it (21 ms) */
            ai->next_think = get_nanos() + (uint64_t) (gen_rand(&server->rand) * 0.4f * (float) NANO_IN_SECOND);
            ai->jet_on        = 0;
            ai->jet_why       = 0;
            ai->perch_until   = 0;
            ai->perch_logged  = 0;
            ai->engage_id     = 0;
            _bot_role_weapon(server, bot, ai);
            continue;
        }
        /* bot_hidden: a roof over the head (the column's top block above it) */
        if ((prof_ticks + bot->id) % 30 == 0 && ai->role != GM_BOT_DEFEND && !ai->camper) {
            int kind = ai->role == GM_BOT_INFILTRATE ? 0 : ai->role == GM_BOT_COVER ? 1 : ai->sneaky ? 2 : 3;
            int top  = map_top(&server->s_map.map, (uint16_t) bot->movement.position.x,
                                            (uint16_t) bot->movement.position.y);
            hidden_of[kind]++;
            hidden_n[kind] += top < (int) bot->movement.position.z - 1;
            if (ai->path_stealth && ai->path_len > 0) {
                hidden_sof[kind]++;
                hidden_sn[kind] += top < (int) bot->movement.position.z - 1;
            }
        }
        if (ai->next_think <= get_nanos()) {
            uint64_t  think_start = get_real_nanos(); /* real: what the tick pays */
            uint8_t   phase_before = ai->phase, camp_before = ai->camp_set;
            ai->aimed      = 0;
            ai->look_cause = NULL;
            ai->move_src   = NULL;
            if (ai->look_ok) {
                ai->look_cur = bot->movement.forward_orientation;
            }
            uint64_t pt0 = get_real_nanos();
            _bot_think(server, bot, ai);
            uint64_t pt1 = get_real_nanos();
            if (!(gamemode_player_rules(server, bot) & GM_RULE_NO_GUN)) { /* a zombie has no gun to snap */
                _bot_snap_shot(server, bot, ai);
            }
            prof_think += pt1 - pt0;
            prof_snap += get_real_nanos() - pt1;
            prof_thinks++;
            _bot_after_think(bot, ai);
            /* stepping aside for a clear line on an enemy behind a wall
               (_bot_fire): whatever step the think chose */
            if (get_nanos() < ai->peek_until && !bot->airborne) {
                ai->move_wx = ai->peek_dx;
                ai->move_wy = ai->peek_dy;
            }
            _bot_keep_space(server, bot, ai, get_nanos());
            _bot_blind_check(server, bot, ai, get_nanos());
            _bot_tunnel_watch(server, bot, ai, get_nanos());
            /* a think long enough to show in the tick time (Tick: in the
               server log): what the bot was doing */
            uint64_t took = get_real_nanos() - think_start;
            if (took > NANO_IN_MILLI * 5) {
                evt_log(bot, "bot_slow", "ms=%.1f phase=%s->%s goal=%s role=%u camp=%u->%u escaping=%u",
                        (double) took / (double) NANO_IN_MILLI, _bot_phase_name(phase_before),
                        _bot_phase_name(ai->phase), ai->mode_goal ? ai->mode_goal : "none", ai->role, camp_before,
                        ai->camp_set, ai->escaping);
            }
        }
        _bots_avoid_grenades(server, bot, ai, get_nanos());
        _bot_jet_brake(server, bot, ai);
        uint64_t tu0 = get_real_nanos();
        _bot_turn(server, bot, ai, get_nanos());
        prof_turn += get_real_nanos() - tu0;
    }
}

/* ------------------------------------------------------------------ */
/* Autofill                                                            */
/* ------------------------------------------------------------------ */

static uint32_t _count_humans(server_t* server)
{
    uint32_t  count = 0;
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (!player->is_bot && player->state != STATE_DISCONNECTED) {
            count++;
        }
    }
    return count;
}

void bots_on_player_join(server_t* server)
{
    (void) server;
    autofill_trim_pending = 1;
}

static void _bots_autofill_trim(server_t* server)
{
    if (!autofill_trim_pending) {
        return;
    }
    autofill_trim_pending = 0;
    if (autofill_target == 0) {
        return;
    }
    while ((_count_humans(server) + bot_count(server)) > autofill_target) {
        player_t *player, *tmp;
        player_t* victim = NULL;
        HASH_ITER(hh, server->players, player, tmp)
        {
            if (player->is_bot) {
                victim = player;
                break;
            }
        }
        if (victim == NULL || !bot_remove(server, victim->id)) {
            break;
        }
    }
}

void bots_on_player_leave(server_t* server)
{
    if (autofill_target == 0) {
        return;
    }
    while ((_count_humans(server) + bot_count(server)) < autofill_target) {
        if (bot_create(server, TEAM_A) == NULL) {
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Difficulty                                                          */
/* ------------------------------------------------------------------ */

static int _bot_difficulty_parse(const char* text)
{
    for (int k = 0; k < BOT_DIFFICULTY_COUNT; k++) {
        if (strcasecmp(text, bot_difficulties[k].name) == 0) {
            return k;
        }
    }
    return -1;
}

void bots_init(server_t* server)
{
    uint8_t level = BOT_DIFFICULTY_NORMAL;
    FILE*   fp    = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = conf != NULL ? toml_table_in(conf, "bots") : NULL;
        /* a preset's difficulty wins over the base [bots] one */
        toml_table_t* presets = conf != NULL ? toml_table_in(conf, "presets") : NULL;
        toml_table_t* preset  = presets != NULL && server->preset_name[0] != '\0'
                               ? toml_table_in(presets, server->preset_name)
                               : NULL;
        toml_table_t* source  = preset != NULL ? preset : t;
        toml_datum_t  d       = source != NULL ? toml_string_in(source, preset != NULL ? "bot_difficulty" : "difficulty")
                                               : (toml_datum_t) {0};
        if (d.ok) {
            int v = _bot_difficulty_parse(d.u.s);
            if (v < 0) {
                LOG_WARNING("[bots] difficulty \"%s\" is not easy, normal or hard: using normal", d.u.s);
            } else {
                level = (uint8_t) v;
            }
            free(d.u.s);
        }
        if (preset != NULL && t != NULL) {
            /* the preset did not name one: the base [bots] still applies */
            toml_datum_t base = toml_string_in(t, "difficulty");
            if (!d.ok && base.ok) {
                int v = _bot_difficulty_parse(base.u.s);
                if (v >= 0) {
                    level = (uint8_t) v;
                }
            }
            if (base.ok) {
                free(base.u.s);
            }
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }
    const char* env = getenv("SP_BOT_DIFFICULTY");
    if (env != NULL && *env != 0) {
        int v = _bot_difficulty_parse(env);
        if (v >= 0) {
            level = (uint8_t) v;
        }
    }
    bot_difficulty_level = level;
    const bot_difficulty_t* p = &bot_difficulties[level];
    LOG_INFO("Bots: difficulty %s (skill %.2f..%.2f, aim error x%.1f)", p->name, p->skill_min, p->skill_max,
             p->aim_error);
    bot_projects_init(server);
    bot_forts_init(server);
}

uint8_t bot_difficulty(void)
{
    return bot_difficulty_level;
}

const char* bot_difficulty_name(uint8_t level)
{
    return level < BOT_DIFFICULTY_COUNT ? bot_difficulties[level].name : "?";
}

void bot_set_difficulty(server_t* server, uint8_t level)
{
    if (level >= BOT_DIFFICULTY_COUNT) {
        return;
    }
    bot_difficulty_level = level;
    /* the bots already playing take the new range too, or changing it in the
       menu would only show after they were replaced */
    player_t *bot, *tmp;
    HASH_ITER(hh, server->players, bot, tmp)
    {
        bot_ai_t* ai = bot->is_bot ? _bot_ai(bot->id) : NULL;
        /* the --bots-test subject keeps its fixed skill */
        if (ai != NULL && !bot->spawn_rect_override) {
            ai->skill = _bot_roll_skill(server);
        }
    }
    LOG_INFO("Bots: difficulty set to %s", bot_difficulties[level].name);
}
