// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <modes/modes.h>
#include <Server/Classes.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Packets/Packets.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Drills.h>
#include <Util/Enums.h>
#include <Util/Weapon.h>
#include <Util/Nanos.h>
#include <Util/Checks/PositionChecks.h>
#include <Server/Nodes.h>
#include <Server/Perks.h>
#include <Server/Plugins.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/World.h>
#include <math.h>
#include <Util/Physics.h>
#include <classes/movement.h>
#include <proto10/codec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>
#include <Util/Log.h>

/* the usual figures of a mode without classes (0.75) */
#define CLASSES_DEFAULT_BLOCKS   50
#define CLASSES_DEFAULT_GRENADES 3
#define CLASSES_DEFAULT_JUMP     (-physics_jump_velocity) /* [gameplay] jump_velocity */

int classes_on(server_t* server)
{
    /* the 0.75 profile has no classes (docs/plan-075-server.md 6.2): every
       player plays the game's default figures and the bots the 0.75 guns.
       gamemode_has already says no there; asked here too so no path of
       this file (the pick, the spawn kit, the wire) gets around it */
    if (profile_is_075()) {
        return 0;
    }
    return gamemode_has(server, GM_FEAT_CLASSES);
}

/* the mode keeps this player without a gun (a zombie): it gets none of the
   class's things, it stays what it is */
static int classes_no_kit(server_t* server, player_t* player)
{
    return (gamemode_player_rules(server, player) & GM_RULE_NO_GUN) != 0;
}

const struct game_class_info* classes_of(server_t* server, player_t* player)
{
    if (player == NULL || !classes_on(server)) {
        return NULL;
    }
    if (classes_no_kit(server, player)) {
        return NULL; /* a zombie stays a zombie */
    }
    const struct game_class_info* c = game_class_get(player->klass);
    return c != NULL ? c : game_class_get(GAME_CLASS_COMMANDO);
}

int classes_blocks(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    if (c == NULL) {
        return CLASSES_DEFAULT_BLOCKS;
    }
    /* a wallet smaller than the start: the start wins nothing, the wallet */
    return c->blocks < c->blocks_max ? c->blocks : c->blocks_max;
}

int classes_blocks_max(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    return c != NULL ? c->blocks_max : CLASSES_DEFAULT_BLOCKS;
}

/* the retail fall: the distance flown down, linear between fall_min and
   fall_max, its full damage from fall_max on; a landing in water takes
   fall_water of it. fall_min == fall_max keeps (the Rocketeer): any fall
   past fall_min takes all of it */
long classes_fall_damage(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    if (c == NULL) {
        player->fall_dist = 0.0f;
        return -1;
    }
    float dist        = player->fall_dist;
    player->fall_dist = 0.0f;
    /* the rule is common/classes/movement.h, the client's prediction too */
    return game_class_fall_damage(c, dist, player->wade);
}

int classes_melee(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    return c != NULL ? c->melee : 0;
}

int classes_grenades(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    if (c == NULL) {
        return CLASSES_DEFAULT_GRENADES;
    }
    return c->gear == GAME_GEAR_GRENADES ? c->gear_count : 0;
}

uint8_t classes_weapon(int klass)
{
    const struct game_class_info* c = game_class_get(klass);
    if (c == NULL) {
        return WEAPON_RIFLE;
    }
    return (uint8_t) c->weapon;
}

int classes_perk(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    if (c == NULL) {
        return -1;
    }
    /* the mode's own perks (the zombie mode's jetpack after two kills) unless
       the classes bring theirs (ngCTF) */
    if (!gamemode_has(server, GM_FEAT_CLASS_PERKS)) {
        return -1;
    }
    switch (c->perk) {
        case GAME_PERK_SENTRY: return P10_PERK_SENTRY;
        case GAME_PERK_LANDMINE: return P10_PERK_LANDMINE;
        case GAME_PERK_DYNAMITE: return P10_PERK_DYNAMITE;
        case GAME_PERK_MG: return P10_PERK_MG;
        default: return P10_PERK_COUNT; /* none of them */
    }
}

int classes_jetpack(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    /* the jetpack on every spawn is the class's own (the Rocketeer), and only
       where the classes own the perks (ngCTF) */
    return c != NULL && c->jetpack && gamemode_has(server, GM_FEAT_CLASS_PERKS);
}

/* the server's table of classes (common/classes/classes.h) */
struct game_class_info* game_class_table(void)
{
    static struct game_class_info table[GAME_CLASSES_COUNT];
    static int                    made = 0;
    if (!made) {
        made = 1;
        for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
            game_class_copy(table + k, game_class_default(k));
        }
    }
    return table;
}

/* config.toml's figures ([classes.<name>]), which every map starts from;
   a plugin's changes last the map */
static struct game_class_info class_config[GAME_CLASSES_COUNT];
static int                    class_config_read = 0;

static int _enabled_count(void)
{
    int n = 0;
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        n += game_class_get(k)->enabled != 0;
    }
    return n;
}

int classes_fallback(void)
{
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        if (game_class_get(k)->enabled) {
            return k;
        }
    }
    return GAME_CLASS_COMMANDO;
}

static void _config_read(void)
{
    class_config_read = 1;
    game_class_reset();
    toml_table_t* root = NULL;
    FILE*         fp   = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char err[200];
        root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
    }
    toml_table_t* all = root != NULL ? toml_table_in(root, "classes") : NULL;
    for (int k = 0; all != NULL && k < GAME_CLASSES_COUNT; k++) {
        const char*   name = game_class_get(k)->name;
        toml_table_t* sub  = toml_table_in(all, name);
        for (int i = 0; sub != NULL && toml_key_in(sub, i) != NULL; i++) {
            const char* key = toml_key_in(sub, i);
            toml_raw_t  raw = toml_raw_in(sub, key);
            char*       str = NULL;
            /* a string is unquoted, a number or a boolean is its text */
            const char* value = raw == NULL ? "" : (toml_rtos(raw, &str) == 0 ? str : raw);
            if (game_class_set(game_class_table() + k, key, value)) {
                LOG_INFO("Classes: %s.%s = %s", name, key, value);
            } else {
                LOG_WARNING("Classes: %s.%s = %s ignored (an unknown key or out of its range)", name, key, value);
            }
            free(str);
        }
    }
    if (root != NULL) {
        toml_free(root);
    }
    if (_enabled_count() == 0) {
        LOG_WARNING("Classes: every class disabled, the Commando stays");
        game_class_table()[GAME_CLASS_COMMANDO].enabled = 1;
    }
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        game_class_copy(class_config + k, game_class_get(k));
    }
}

void classes_map_start(server_t* server)
{
    (void) server;
    if (!class_config_read) {
        _config_read();
    }
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        game_class_copy(game_class_table() + k, class_config + k);
    }
}

static void _send_info(server_t* server, player_t* to, int k, const char* key)
{
    char n[8], value[128];
    snprintf(n, sizeof n, "%d", k);
    if (!game_class_value(game_class_get(k), key, value, sizeof value)) {
        return;
    }
    const char* parts[] = {"class-info", n, key, value};
    proto10_bridge_generic(server, to, 4, parts);
}

int classes_set(server_t* server, int k, const char* key, const char* value, const char* by)
{
    if (game_class_get(k) == NULL) {
        return 0;
    }
    struct game_class_info test;
    game_class_copy(&test, game_class_get(k));
    if (!game_class_set(&test, key, value)) {
        LOG_WARNING("Classes: %s.%s = %s refused (%s: an unknown key or out of its range)", game_class_get(k)->name,
                    key, value, by);
        return 0;
    }
    if (!test.enabled && game_class_get(k)->enabled && _enabled_count() == 1) {
        LOG_WARNING("Classes: %s is the last class enabled, kept (%s)", game_class_get(k)->name, by);
        return 0;
    }
    game_class_copy(game_class_table() + k, &test);
    char shown[128];
    game_class_value(game_class_get(k), key, shown, sizeof shown);
    LOG_INFO("Classes: %s.%s = %s (%s)", game_class_get(k)->name, key, shown, by);
    /* the clients predict the speed and the jump and show the figures:
       every change reaches them at once */
    if (classes_on(server)) {
        _send_info(server, NULL, k, key);
    }
    return 1;
}

static void _broadcast_class(server_t* server, player_t* player)
{
    char id[8], n[8];
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    snprintf(n, sizeof n, "%u", (unsigned) player->klass);
    const char* parts[] = {"class", id, n};
    proto10_bridge_generic(server, NULL, 3, parts);
}

int classes_pick(server_t* server, player_t* player, int klass, const char* by)
{
    const char* why = NULL;
    if (profile_is_075()) {
        why = "profile";
    } else if (!classes_on(server)) {
        why = "mode";
    } else if (classes_of(server, player) == NULL) {
        why = "mode"; /* a zombie stays a zombie: no class for it */
    } else if (game_class_get(klass) == NULL) {
        why = "bad_value";
    } else if (!game_class_get(klass)->enabled) {
        why = "disabled";
    } else if (strcmp(by, "plugin") != 0 && player->klass != klass && !plugins_on_class(server, player, klass, by)) {
        why = "plugin";
    }
    if (why != NULL) {
        evt_log(player, "class", "action=refused class=%d by=%s why=%s", klass, by, why);
        return 0;
    }
    if (player->klass == klass) {
        return 1;
    }
    player->klass = (uint8_t) klass;
    evt_log(player, "class", "action=pick class=%s by=%s alive=%d", game_class_get(klass)->name, by,
            player->state == STATE_READY && player->alive);
    /* the class's placed perks and its unused charge belong to the class
       the player is leaving */
    perks_class_changed(server, player);
    proto10_objects_owner_class_changed(server, player);
    _broadcast_class(server, player);
    /* a living player respawns with it, the way a weapon change does
       (game_player_change_weapon); a dead one or one still joining takes it at
       the next spawn */
    if (player->state == STATE_READY && player->alive && player->team != TEAM_SPECTATOR) {
        send_kill_action_packet(server, player, player, 6, 5, 0);
        player->state = STATE_WAITING_FOR_RESPAWN;
    } else if (player->weapon != classes_weapon(klass)) {
        player->weapon = classes_weapon(klass);
        set_default_player_ammo(player);
    }
    return 1;
}

/* the second gun, full, put away */
static void _alt_ready(player_t* player, int weapon)
{
    player->alt_weapon = 255;
    /* a spawn always starts with the class's primary in hand */
    player->gun_slot = 0;
    if (weapon < 0 || weapon == (int) player->weapon) {
        return;
    }
    uint8_t keep = player->weapon, clip = player->weapon_clip, reserve = player->weapon_reserve;
    uint8_t dclip = (uint8_t) player->default_clip, dreserve = (uint8_t) player->default_reserve;
    player->weapon = (uint8_t) weapon;
    set_default_player_ammo(player);
    player->alt_weapon          = player->weapon;
    player->alt_clip            = player->weapon_clip;
    player->alt_reserve         = player->weapon_reserve;
    player->alt_default_clip    = (uint8_t) player->default_clip;
    player->alt_default_reserve = (uint8_t) player->default_reserve;
    player->weapon              = keep;
    player->weapon_clip         = clip;
    player->weapon_reserve      = reserve;
    player->default_clip        = dclip;
    player->default_reserve     = dreserve;
}

int classes_swap_gun(server_t* server, player_t* player, const char* by)
{
    uint64_t    now = get_nanos();
    const char* why = NULL;
    if (classes_of(server, player) == NULL) {
        why = "mode";
    } else if (player->alt_weapon == 255) {
        why = "no_second_gun";
    } else if (player->state != STATE_READY || !player->alive) {
        why = "not_alive";
    } else if (player->swapped_at != 0 && now - player->swapped_at < NANO_IN_MILLI * 250) {
        why = "rate";
    }
    if (why != NULL) {
        evt_log(player, "class", "action=swap_refused why=%s by=%s", why, by);
        return 0;
    }
    uint8_t w = player->weapon, c = player->weapon_clip, r = player->weapon_reserve;
    uint8_t dc = (uint8_t) player->default_clip, dr = (uint8_t) player->default_reserve;
    player->weapon              = player->alt_weapon;
    player->weapon_clip         = player->alt_clip;
    player->weapon_reserve      = player->alt_reserve;
    player->default_clip        = player->alt_default_clip;
    player->default_reserve     = player->alt_default_reserve;
    player->alt_weapon          = w;
    player->alt_clip            = c;
    player->alt_reserve         = r;
    player->alt_default_clip    = dc;
    player->alt_default_reserve = dr;
    /* a reload of the gun put away is lost, as a tool change loses it */
    player->reloading  = 0;
    player->swapped_at = now;
    set_default_player_pellets(player);
    evt_log(player, "class", "action=swap weapon=%u clip=%u reserve=%u by=%s", (unsigned) player->weapon,
            (unsigned) player->weapon_clip, (unsigned) player->weapon_reserve, by);
    /* nobody is told with a GenericCommand: the tool slot of the body's
       entity carries which gun is in hand (Proto10/Bridge.c) */
    player->gun_slot ^= 1;
    return 1;
}

/* picks which of the class's guns is in hand (1.0's Weapon1 / Weapon2); a
   swap only when it is the other one. 0 when refused (no second gun, dead,
   rate) */
int classes_select_gun(server_t* server, player_t* player, int slot, const char* by)
{
    if (slot == (int) player->gun_slot) {
        return 1;
    }
    return classes_swap_gun(server, player, by);
}

void classes_on_spawn(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    if (c == NULL || player->team == TEAM_SPECTATOR) {
        return;
    }
    /* a class the server turned off since: the first one still on */
    if (!c->enabled) {
        player->klass = (uint8_t) classes_fallback();
        evt_log(player, "class", "action=pick class=%s by=disabled alive=0", game_class_get(player->klass)->name);
        _broadcast_class(server, player);
        c = classes_of(server, player);
    }
    uint8_t weapon = classes_weapon(player->klass);
    if (player->weapon != weapon) {
        player->weapon = weapon;
    }
    set_default_player_ammo(player);
    _alt_ready(player, c->secondary);
    /* the wallet's start of the class (retail starting_blocks); the restock
       (the tent) fills it to blocks_max, and every dig gives one back */
    player->blocks = (uint16_t) classes_blocks(server, player);
    /* a mode that carries no blocks (the arena): nothing in the inventory */
    if (gamemode_player_rules(server, player) & GM_RULE_NO_BLOCK) {
        player->blocks = 0;
    }
    player->grenades = (uint8_t) classes_grenades(server, player);
    player->drill_at = 0;
    player->fall_dist = 0.0f;
    classes_restock(server, player);
    /* the Marksman's landmine from the start of every life */
    if (c->spawn_perk) {
        perks_class_spawn(server, player);
    }
    evt_log(player, "class", "action=spawn class=%s weapon=%u blocks=%u wallet=%d grenades=%d health=%d", c->name,
            (unsigned) player->weapon, (unsigned) player->blocks, c->blocks_max, (int) player->grenades, c->health);
}

void classes_movement(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    /* the fallbacks for a player with no class are common/classes/
       movement.h, the same the client predicts with */
    player->class_speed   = game_class_speed(c);
    player->class_sprint  = game_class_sprint(c);
    player->class_water   = game_class_water_friction(c);
    player->jump_velocity = game_class_jump(c, CLASSES_DEFAULT_JUMP);
    /* the mode's high jump (the zombies: never a class) jumps higher on a
       tap too; the client predicts it in classes_jump */
    if (c == NULL && (gamemode_player_rules(server, player) & GM_RULE_HIGH_JUMP)) {
        player->jump_velocity = GAME_ZOMBIE_TAP_JUMP_VELOCITY;
    }
}

void classes_client_in_game(server_t* server, player_t* player)
{
    if (!classes_on(server)) {
        return;
    }
    const char* on[] = {"classes", "1"};
    proto10_bridge_generic(server, player, 2, on);
    /* the figures this server plays them with */
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        for (int i = 0; i < GAME_CLASS_KEYS; i++) {
            _send_info(server, player, k, game_class_key(i));
        }
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state == STATE_DISCONNECTED) {
            continue;
        }
        char id[8], n[8];
        snprintf(id, sizeof id, "%u", (unsigned) p->id);
        snprintf(n, sizeof n, "%u", (unsigned) p->klass);
        const char* parts[] = {"class", id, n};
        proto10_bridge_generic(server, player, 3, parts);
    }
}

/* tells every 1.0 client the classes are on (or off) and the figures they
   play them with: the host turned them on or off in the middle of a match */
void classes_tell_all(server_t* server, int on)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state == STATE_DISCONNECTED) {
            continue;
        }
        if (on) {
            classes_client_in_game(server, p);
        } else {
            const char* off[] = {"classes", "0"};
            proto10_bridge_generic(server, p, 2, off);
        }
    }
}

int classes_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    if (strcmp(command, "class") != 0) {
        return 0;
    }
    char* end = NULL;
    long  k   = strtol(arg, &end, 10);
    if (arg[0] == '\0' || end == NULL || *end != '\0') {
        evt_log(player, "class", "action=refused class=%s by=client why=bad_value", arg);
        return 1;
    }
    classes_pick(server, player, (int) k, "client");
    return 1;
}

/* hp stays 0..100 on the wire: a class with more health takes every hit
   scaled down, one with less scaled up (rounded up, like the zombie's, so a
   pellet still hurts); the retail profile also gives each class its own
   head shot taken (the Marksman's and Rocketeer's bodies take one and a
   half, the Miner's a half) */
long classes_hit_damage(server_t* server, player_t* attacker, player_t* victim, long damage, uint8_t kill_reason)
{
    (void) attacker;
    const struct game_class_info* c = classes_of(server, victim);
    if (c == NULL || damage <= 0) {
        return damage;
    }
    if (kill_reason == 1 && c->headshot != 1.0f) { /* 1: a shot to the head (Hit.c) */
        damage = (long) ceilf((float) damage * c->headshot);
        if (damage < 1) {
            damage = 1;
        }
    }
    if (c->health == 100) {
        return damage;
    }
    return (damage * 100 + c->health - 1) / c->health;
}

int classes_dig(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    return c != NULL ? c->dig : 1;
}

uint64_t classes_dig_delay(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    /* seconds, the classic 1 without a class (common/classes/movement.h) */
    return (uint64_t) (game_class_dig_delay(c) * 1000.0f) * NANO_IN_MILLI;
}

int classes_block_damage(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    return c != NULL && c->block_damage > 0 ? c->block_damage : game_weapon_block_damage(player->weapon);
}

int classes_gun_breaks(server_t* server, player_t* player)
{
    const struct game_class_info* c = classes_of(server, player);
    if (c == NULL || c->block_damage <= 0) {
        return 1;
    }
    int pellets = player->weapon == WEAPON_SHOTGUN ? 8 : 1; /* set_default_player_pellets */
    int n       = pellets * c->block_damage / GAME_BLOCK_BREAK;
    return n > 1 ? n : 1;
}

int classes_cell_diggable(server_t* server, int x, int y, int z)
{
    map_store_t* map = &server->s_map.map;
    return x >= 0 && y >= 0 && z >= 0 && x < map->size_x && y < map->size_y && z < 62 &&
           map_solid(map, x, y, z) && gamemode_cell_editable(server, x, y, z);
}

void classes_check_shell(server_t* server, int cx, int cy, int cz, int r)
{
    int s = r + 1;
    for (int z = cz - s; z <= cz + s; z++) {
        for (int x = cx - s; x <= cx + s; x++) {
            for (int y = cy - s; y <= cy + s; y++) {
                if (abs(x - cx) != s && abs(y - cy) != s && abs(z - cz) != s) {
                    continue;
                }
                vector3i_t n = {x, y, z};
                if (z < 62 && valid_pos_v3i(server, n) && map_solid(&server->s_map.map, x, y, z)) {
                    check_node(server, n);
                }
            }
        }
    }
}

static void _drill_tell(server_t* server, player_t* player)
{
    char        n[8];
    const char* parts[] = {"drill-charges", n};
    snprintf(n, sizeof n, "%u", (unsigned) player->drill_charges);
    proto10_bridge_generic(server, player, 2, parts);
}

void classes_restock(server_t* server, player_t* player)
{
    if (player->alt_weapon != 255) {
        player->alt_reserve = player->alt_default_reserve;
    }
    const struct game_class_info* c = classes_of(server, player);
    uint8_t charges = c != NULL && c->gear == GAME_GEAR_DRILL ? (uint8_t) c->gear_count : 0;
    if (charges != player->drill_charges) {
        player->drill_charges = charges;
        _drill_tell(server, player);
    }
}

int classes_drill_base(server_t* server, uint8_t team, float x, float y)
{
    if (team > 1) {
        return 0;
    }
    gamemode_vars_t* gm = &server->protocol.gamemode;
    float            bx = gm->base[team].x - x, by = gm->base[team].y - y;
    if (bx * bx + by * by < DRILL_BASE_R * DRILL_BASE_R) {
        return 1;
    }
    /* the intel where it lies (carried off, it is no base) */
    float ix = gm->intel[team].x - x, iy = gm->intel[team].y - y;
    return !gm->intel_held[team] && ix * ix + iy * iy < DRILL_BASE_R * DRILL_BASE_R;
}

int classes_drill(server_t* server, player_t* player, vector3f_t dir, const char* by)
{
    const struct game_class_info* c   = classes_of(server, player);
    uint64_t                      now = get_nanos();
    const char*                   why = NULL;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 0.5f) {
        dir = player->movement.forward_orientation; /* no direction given: the look */
        len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    }
    if (c == NULL) {
        why = "mode";
    } else if (c->gear != GAME_GEAR_DRILL) {
        why = "class";
    } else if (player->state != STATE_READY || !player->alive || player->team == TEAM_SPECTATOR) {
        why = "not_alive";
    } else if (player->item != TOOL_GRENADE) {
        why = "tool";
    } else if (player->drill_charges == 0) {
        why = "no_charges";
    } else if (player->drill_at != 0 && now - player->drill_at < (uint64_t) (DRILL_INTERVAL * NANO_IN_SECOND)) {
        why = "rate";
    } else if (len < 0.5f) {
        why = "direction";
    } else if (classes_drill_base(server, player->team, player->movement.position.x, player->movement.position.y)) {
        /* not in its own base: the bits bored its tent and its intel's
           ground away */
        why = "base";
    }
    if (why != NULL) {
        evt_log(player, "class", "action=drill_refused why=%s by=%s charges=%u", why, by,
                (unsigned) player->drill_charges);
        const char* parts[] = {"drill-refused", why};
        proto10_bridge_generic(server, player, 2, parts);
        return 0;
    }
    dir.x /= len, dir.y /= len, dir.z /= len;
    vector3f_t eye = player->movement.eye_pos;
    if (!plugins_on_destroy(server, player, (int) eye.x, (int) eye.y, (int) eye.z, "drill")) {
        evt_log(player, "class", "action=drill_refused why=plugin by=%s", by);
        return 0;
    }
    player->drill_charges--;
    player->drill_at = now;
    /* a drill bit, flown by the server (Proto10/Drills.c): it falls in
       the air and bores 3 x 3 through the ground. It bored the whole way at
       once before, and a charge fired at the air or the water's level bored
       nothing */
    drills_fire(server, player, dir, by);
    evt_log(player, "class", "action=drill by=%s eye=%.1f,%.1f,%.1f dir=%.2f,%.2f,%.2f charges=%u", by, eye.x, eye.y,
            eye.z, dir.x, dir.y, dir.z, (unsigned) player->drill_charges);
    _drill_tell(server, player);
    return 1;
}
