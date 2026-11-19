#include <unistd.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Classes.h>
#include <Server/Proto10/Rockets.h>
#include <Server/Plugins.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Bots.h>
#include <Server/EventLog.h>
#include <Server/Grenade.h>
#include <Server/HostPackets.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Entities.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Proto10/Session.h>
#include <Server/Proto10/World.h>
#include <Server/Conduct.h>
#include <Server/LagComp.h>
#include <Server/SpawnProtect.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/DataDir.h>
#include <Util/Alloc.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>
#include <modes/modes.h>
#include <Util/JSONHelpers.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <Util/Physics.h>
#include <Util/Queue.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Util/Weapon.h>
#include <enet/enet.h>
#include <json-c/json_util.h>
#include <math.h>
#include <proto10/codec.h>
#include <stdint.h>
#include <time.h>

/* The player id handed to a new connection, or 0xFF when the server is full.
   The id is the first one no player holds; the caller has already checked
   that a slot is free. */
static uint8_t _plr_next_free_id(server_t* server)
{
    if (server->protocol.num_players == server->protocol.max_players) {
        return 0xFF;
    }
    uint8_t player_id;
    for (player_id = 0; player_id < server->protocol.max_players; ++player_id) {
        player_t* player;
        HASH_FIND(hh, server->players, &player_id, sizeof(player_id), player);
        if (player == NULL) {
            break;
        }
    }
    server->protocol.num_players++;
    return player_id;
}

int player_sort(player_t* a, player_t* b)
{
    if (a->id < b->id) {
        return -1;
    }
    if (a->id > b->id) {
        return 1;
    }
    return 0;
}

/* A player's queue of map cells is a doubly linked list of separately
   allocated nodes; empty it wherever a player is torn down or reset. */
static void _plr_free_map_queue(player_t* player)
{
    if (player->map_queue == NULL) {
        return;
    }
    queue_t *node, *tmp;
    DL_FOREACH_SAFE(player->map_queue, node, tmp)
    {
        free(node->block);
        DL_DELETE(player->map_queue, node);
        free(node);
    }
    player->map_queue = NULL;
}

void free_all_players(server_t* server)
{
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        _plr_free_map_queue(player);
        HASH_DEL(server->players, player);
        free(player);
    }
}

/* The per tick movement of one ready player: physics, jetpack, fall and water
   damage, and the leap that goes into the physics step. */
static void _plr_step_movement(server_t* server, player_t* player)
{
    long    falldamage    = 0;
    uint8_t was_thrusting = player->jetpack_thrusting;
    float   fuel_before   = player->jetpack_fuel;
    unsigned rules        = gamemode_player_rules(server, player);
    uint64_t now          = get_nanos();
    /* the high jump has a cooldown: between two a zombie jumps like anyone
       (it reached any roof every second) */
    player->high_jump = (rules & GM_RULE_HIGH_JUMP) != 0 && now >= player->zombie_leap_ready;
    player->fast      = (rules & GM_RULE_FAST) != 0;
    player->slowed    = player->fast && now < player->zombie_slow_until;
    classes_movement(server, player);
    if (player->leap_charge) {
        player->jumping = 1; /* the leap asked for (Bridge.c, the bots) */
    }
    if (player->high_jump && player->jumping && player->leap_charge) {
        player->zombie_leap_ready =
            now + (uint64_t) ((GAME_ZOMBIE_LEAP_COOLDOWN - GAME_ZOMBIE_LEAP_SLACK) * NANO_IN_SECOND);
        evt_log(player, "zombie", "action=leap_used charge=%.2f ready_in=%.1f",
                game_zombie_leap_charge(player->leap_charge), GAME_ZOMBIE_LEAP_COOLDOWN);
    } else if (player->leap_charge) {
        evt_log(player, "zombie", "action=leap_refused why=%s ready_in=%.2f",
                (rules & GM_RULE_HIGH_JUMP) ? "cooldown" : "rules",
                (double) ((int64_t) (player->zombie_leap_ready - now)) / NANO_IN_SECOND);
    }
    uint8_t jumped = player->jumping && !player->flying;
    falldamage     = physics_move_player(server, player, &server->physics);
    if (jumped) {
        proto10_bridge_jump(server, player);
    }
    if (rules & GM_RULE_NO_FALL) {
        falldamage = 0;
    }
    jetpack_after_move(server, player, was_thrusting, fuel_before);
    if (falldamage > 0) {
        vector3f_t zero = {0, 0, 0};
        send_set_hp(server, player, player, falldamage, 0, 4, 5, 0, zero);
    }
    if (server->protocol.gamemode.water_damage_enabled && player->wade && player->alive) {
        uint64_t time = get_nanos();
        if (time - player->timers.since_last_water_damage >= NANO_IN_SECOND) {
            vector3f_t zero = {0, 0, 0};
            send_set_hp(server, player, player, server->protocol.gamemode.water_damage, 0, 4, 5, 0, zero);
            player->timers.since_last_water_damage = time;
        }
    }
}

void update_movement_and_grenades(server_t* server)
{
    server->physics.ftotclk =
    (server->global_timers.update_time - server->global_timers.time_since_start) / 1000000000.f;
    server->physics.fsynctics =
    (server->global_timers.update_time - server->global_timers.last_update_time) / 1000000000.f;
    /* one step of at most 1/20 s: after a stall (the process starved of CPU,
       100 ms physics ticks) the whole delay applied at once made falls of
       one block deal 100 damage, and players and bots died where they stood
       (48 deaths in 240 s on babelstairs). A stall is now a moment of slow
       motion instead */
    if (server->physics.fsynctics > 0.05f) {
        server->physics.fsynctics = 0.05f;
    }
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (player->state == STATE_READY) {
            /* a seat holds the body: no physics, no fall (Proto10/Mount.c) */
            if (mount_player_step(server, player)) {
                handle_grenade(server, player);
                continue;
            }
            _plr_step_movement(server, player);
        }
        handle_grenade(server, player);
    }
}

uint8_t get_player_unstuck(server_t* server, player_t* player)
{
    for (float z = player->movement.prev_legit_pos.z - 1; z <= player->movement.prev_legit_pos.z + 1; z++) {
        if (valid_player_pos(server, player, player->movement.prev_legit_pos.x, player->movement.prev_legit_pos.y, z)) {
            player->movement.prev_legit_pos.z = z;
            player->movement.position         = player->movement.prev_legit_pos;
            return 1;
        }
        for (float x = player->movement.prev_legit_pos.x - 1; x <= player->movement.prev_legit_pos.x + 1; x++) {
            for (float y = player->movement.prev_legit_pos.y - 1; y <= player->movement.prev_legit_pos.y + 1; y++) {
                if (valid_player_pos(server, player, x, y, z)) {
                    player->movement.prev_legit_pos.x = x;
                    player->movement.prev_legit_pos.y = y;
                    player->movement.prev_legit_pos.z = z;
                    player->movement.position         = player->movement.prev_legit_pos;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* The bottom layer (z = size_z - 1) is the water plane: it is solid in the map
   data but physics treats it as empty (see clipworld), so a player standing on
   it sinks below z = 61 and wades. A column whose first solid block is that
   layer is open water. */
uint8_t is_water_column(server_t* server, uint16_t x, uint16_t y)
{
    if (x >= server->s_map.map.size_x || y >= server->s_map.map.size_y) {
        return 0;
    }
    return map_top(&server->s_map.map, x, y) >= server->s_map.map.size_z - 1;
}

/* A surface with 3 blocks free and a block under */
static uint8_t _plr_valid_spawn(server_t* server, uint16_t x, uint16_t y, uint16_t z, uint8_t allow_water)
{
    if (!allow_water && z + 1 >= server->s_map.map.size_z - 1) {
        return 0; /* the floor would be the water plane */
    }
    return   map_solid(&server->s_map.map, x, y, z + 1) &&
            !map_solid(&server->s_map.map, x, y, z)     &&
            !map_solid(&server->s_map.map, x, y, z - 1) &&
            !map_solid(&server->s_map.map, x, y, z - 2);
}

/* Nearest valid spawn height in the column, searching down from start_z and
   then up. Returns 1 and fills z_out when the column has one. */
static uint8_t _plr_find_spawn_z(server_t* server, float x, float y, float start_z, uint8_t allow_water, float* z_out)
{
    if (x < 0.f || y < 0.f || x >= server->s_map.map.size_x || y >= server->s_map.map.size_y) {
        return 0;
    }
    uint16_t start = (start_z < 0.f) ? 0 : (uint16_t) start_z;
    for (uint16_t z = start; z < server->s_map.map.size_z; z++) {
        if (_plr_valid_spawn(server, (uint16_t) x, (uint16_t) y, z, allow_water)) {
            *z_out = z - 2.4F;
            return 1;
        }
    }
    for (uint16_t z = start; z > 0; z--) {
        if (_plr_valid_spawn(server, (uint16_t) x, (uint16_t) y, z, allow_water)) {
            *z_out = z - 2.4F;
            return 1;
        }
    }
    return 0;
}

#define SPAWN_DRY_ATTEMPTS  32
#define SPAWN_SEARCH_RADIUS 128

/* a dry spawn cell around x, y (a squadmate), nearest first; 0 when none */
uint8_t player_spawn_near(server_t* server, float x, float y, float z, vector3f_t* out)
{
    for (int r = 1; r <= 6; r++) {
        int steps = 8 * r;
        for (int s = 0; s < steps; s++) {
            float a  = (float) s * 2.f * (float) M_PI / (float) steps;
            float px = floorf(x + cosf(a) * (float) r) + 0.5f;
            float py = floorf(y + sinf(a) * (float) r) + 0.5f;
            float pz;
            /* close to the mate's height: not on the roof above it */
            if (_plr_find_spawn_z(server, px, py, z, 0, &pz) && fabsf(pz - z) < 3.0f) {
                out->x = px;
                out->y = py;
                out->z = pz;
                return 1;
            }
        }
    }
    return 0;
}

/* the body position of a map spawn point: the author's floor, or the next
   one down/up when that ground was dug away or built over */
uint8_t player_spawn_on_point(server_t* server, vector3f_t point, vector3f_t* out)
{
    float z;
    if (point.x < 0.f || point.y < 0.f || !_plr_find_spawn_z(server, point.x, point.y, point.z - 1.f, 0, &z)) {
        return 0;
    }
    out->x = point.x;
    out->y = point.y;
    out->z = z;
    return 1;
}

void player_restock(server_t* server, player_t* player)
{
    player->hp       = 100;
    player->grenades = (uint8_t) classes_grenades(server, player);
    /* the retail block crate: the wallet back to its class max (a spawn
       starts from the class's own smaller starting blocks) */
    player->blocks = (uint16_t) classes_blocks_max(server, player);
    /* a mode that carries no blocks (the arena): nothing in the inventory */
    if (gamemode_player_rules(server, player) & GM_RULE_NO_BLOCK) {
        player->blocks = 0;
    }
    proto10_bridge_wallet(server, player);
    set_default_player_ammo_reserve(player);
    classes_restock(server, player);
    const char* parts[] = {"restock"};
    proto10_bridge_generic(server, player, 1, parts);
}

/* A spawn faces the mode's objective, level (the hill, the enemy intel, the
   front territory). It looked along +x or -x by team whatever the map, and in
   king of the hill a human was born with the hill behind it. The look goes
   to the client in the body's Create, which a 1.0 client now takes */
void player_face_objective(server_t* server, player_t* player)
{
    if (player->team != TEAM_A && player->team != TEAM_B) {
        return;
    }
    float tx = player->movement.position.x, ty = player->movement.position.y;
    gamemode_spawn_facing(server, player, &tx, &ty);
    float dx = tx - player->movement.position.x, dy = ty - player->movement.position.y;
    float d  = sqrtf(dx * dx + dy * dy);
    if (!(d > 0.5f)) {
        /* on the objective already (or none): the team's old way */
        dx = player->team == TEAM_A ? 1.0f : -1.0f;
        dy = 0.0f;
        d  = 1.0f;
    }
    vector3f_t look = {dx / d, dy / d, 0.0f};
    physics_reorient_player(player, &look);
    evt_log(player, "spawn_face", "at=%.0f,%.0f to=%.0f,%.0f", player->movement.position.x,
            player->movement.position.y, tx, ty);
}

/* A spawn beside the player it was made for (the tutorial's target, ~6 blocks
   in front of it). Places the player when the mate is alive and a dry cell
   sits there; 0 to fall through to the team's rectangle. */
static uint8_t _plr_spawn_beside(server_t* server, player_t* player)
{
    if (player->spawn_beside == 0) {
        return 0;
    }
    player_t*  next = NULL;
    uint8_t    id   = (uint8_t) (player->spawn_beside - 1);
    vector3f_t at;
    HASH_FIND(hh, server->players, &id, sizeof(id), next);
    float fx = 0.f, fy = 0.f, fl = 0.f;
    if (next != NULL) {
        fx = next->movement.forward_orientation.x;
        fy = next->movement.forward_orientation.y;
        fl = sqrtf(fx * fx + fy * fy);
    }
    if (next != NULL && next->alive && next->state == STATE_READY &&
        player_spawn_near(server, next->movement.position.x + (fl > 0.1f ? fx / fl * 6.f : 6.f),
                          next->movement.position.y + (fl > 0.1f ? fy / fl * 6.f : 0.f),
                          next->movement.position.z, &at))
    {
        player->movement.position   = at;
        player->movement.velocity.x = player->movement.velocity.y = player->movement.velocity.z = 0.f;
        return 1;
    }
    return 0;
}

/* The team rectangle's spawn, with its fallbacks: the map's listed points,
   dry picks inside the rectangle, a ring search around it, then water. */
static void _plr_spawn_from_rect(server_t* server, player_t* player)
{
    /* a bot flagged with spawn_rect_override lands on the other team's
       rectangle: the --bots-test subject and the tutorial's target are met
       at once */
    uint8_t   rect_team = player->spawn_rect_override ? (player->team ^ 1) : player->team;
    quad3d_t* spawn     = server->protocol.spawns + rect_team;

    float dx = spawn->to.x - spawn->from.x;
    float dy = spawn->to.y - spawn->from.y;
    float dz = spawn->to.z - spawn->from.z;

    player->movement.velocity.x = 0.f;
    player->movement.velocity.y = 0.f;
    player->movement.velocity.z = 0.f;

    player->movement.forward_orientation.x = 0.f;
    player->movement.forward_orientation.y = 0.f;
    player->movement.forward_orientation.z = 0.f;

    /* the map's author listed spawn points (pyspades get_spawn_location):
       one of them, on the floor the author named or, when that ground
       was dug away, the next one down. Same dry rule as the rectangle */
    uint8_t    meta_team = rect_team;
    vector3f_t point;
    for (uint8_t attempt = 0; attempt < 8 && map_meta_spawn(server, meta_team, &point); attempt++) {
        if (player_spawn_on_point(server, point, &player->movement.position)) {
            return;
        }
        if (attempt == 7) {
            evt_log(player, "spawn_search", "src=map_points why=no-dry-point fallback=rect");
        }
    }

    /* Spawn rectangles can lie mostly in the sea (the default west/east
       areas are ~63% water on Pinpoint), and a single random pick used to
       drop players straight into it. Retry inside the rectangle keeping
       only dry ground, and widen the search only when it has none. */
    float x = 0.f, y = 0.f, z = 0.f;
    for (uint8_t attempt = 0; attempt < SPAWN_DRY_ATTEMPTS; attempt++) {
        x = spawn->from.x + dx * gen_rand(&server->rand) + 0.5F;
        y = spawn->from.y + dy * gen_rand(&server->rand) + 0.5F;
        z = spawn->from.z + dz * gen_rand(&server->rand);
        if (_plr_find_spawn_z(server, x, y, z, 0, &player->movement.position.z)) {
            player->movement.position.x = x;
            player->movement.position.y = y;
            return;
        }
    }

    /* nearest dry ground around the rectangle centre, ring by ring */
    float cx = spawn->from.x + dx * 0.5f + 0.5F;
    float cy = spawn->from.y + dy * 0.5f + 0.5F;
    for (int r = 2; r <= SPAWN_SEARCH_RADIUS; r += 2) {
        int steps = 8 + r; /* keep the ring sampling dense as it grows */
        for (int s = 0; s < steps; s++) {
            float a  = (float) s * 2.f * (float) M_PI / (float) steps;
            float px = cx + cosf(a) * (float) r;
            float py = cy + sinf(a) * (float) r;
            if (_plr_find_spawn_z(server, px, py, spawn->from.z, 0, &player->movement.position.z)) {
                player->movement.position.x = px;
                player->movement.position.y = py;
                evt_log(player, "spawn_search", "dry=ring r=%d rect_center=%.0f,%.0f", r, cx, cy);
                return;
            }
        }
    }

    /* an all-water map (Empty_ocean): wading beats spawning in the sky */
    player->movement.position.x = x;
    player->movement.position.y = y;
    if (_plr_find_spawn_z(server, x, y, z, 1, &player->movement.position.z)) {
        evt_log(player, "spawn_search", "dry=none water=accepted");
        return;
    }

    /* no land anywhere: put the body at the top of the map */
    player->movement.position.z = -2.4f;
}

void set_player_respawn_point(server_t* server, player_t* player)
{
    if (player->team == TEAM_SPECTATOR) {
        return;
    }
    /* a plugin's spot first (on_spawn_point), then the mode's */
    if (plugins_on_spawn_point(server, player)) {
        return;
    }
    /* the mode may place the player itself (CTF squads: next to a mate) */
    if (gamemode_spawn_point(server, player)) {
        player->movement.velocity.x = player->movement.velocity.y = player->movement.velocity.z = 0.f;
        return;
    }
    /* in front of the player it was made for, ~6 blocks off (the
       tutorial's target: in sight, out of a rocket's own blast) */
    if (_plr_spawn_beside(server, player)) {
        return;
    }
    _plr_spawn_from_rect(server, player);
}

/* The list of roles /login accepts, copied into a new player. */
static void _plr_assign_roles(server_t* server, player_t* player)
{
    permissions_t roles[5] = {{"manager", &server->manager_passwd, 4},
                              {"admin", &server->admin_passwd, 3},
                              {"mod", &server->mod_passwd, 2},
                              {"guard", &server->guard_passwd, 1},
                              {"trusted", &server->trusted_passwd, 0}};
    for (unsigned long i = 0; i < sizeof(roles) / sizeof(permissions_t); ++i) {
        player->role_list[i] = roles[i];
    }
}

/* A reset drops every grenade the player held and tells the 1.0 clients. */
static void _plr_free_grenades(server_t* server, player_t* player)
{
    grenade_t *grenade, *tmp;
    DL_FOREACH_SAFE(player->grenade, grenade, tmp)
    {
        proto10_entities_grenade_removed(server, player, grenade);
        DL_DELETE(player->grenade, grenade);
        free(grenade);
    }
}

void init_player(server_t*  server,
                 player_t*  player,
                 uint8_t    reset,
                 uint8_t    disconnect,
                 vector3f_t empty,
                 vector3f_t forward,
                 vector3f_t strafe,
                 vector3f_t height)
{
    if (reset == 0) {
        player->state = STATE_DISCONNECTED;
    }
    _plr_free_map_queue(player);
    player->ups                          = 60;
    player->timers.time_since_last_wu    = get_nanos();
    player->input                        = 0;
    player->movement.eye_pos             = empty;
    player->movement.forward_orientation = forward;
    player->movement.strafe_orientation  = strafe;
    player->movement.height_orientation  = height;
    player->movement.position            = empty;
    player->movement.velocity            = empty;
    if (reset == 0 && disconnect == 0) {
        _plr_assign_roles(server, player);
        player->grenade = NULL;
    }
    player->airborne                             = 0;
    player->wade                                 = 0;
    player->lastclimb                            = 0;
    player->move_backwards                       = 0;
    player->move_forward                         = 0;
    player->move_left                            = 0;
    player->move_right                           = 0;
    player->jumping                              = 0;
    player->crouching                            = 0;
    player->sneaking                             = 0;
    player->sprinting                            = 0;
    player->primary_fire                         = 0;
    player->secondary_fire                       = 0;
    player->can_build                            = 1;
    player->allow_killing                        = 1;
    player->allow_team_killing                   = 0;
    player->muted                                = 0;
    player->team                                 = TEAM_SPECTATOR;
    player->timers.since_last_base_enter         = 0;
    player->timers.since_last_base_enter_restock = 0;
    player->timers.since_last_3block_dest        = 0;
    player->timers.since_last_block_dest         = 0;
    player->timers.since_last_block_plac         = 0;
    player->timers.since_last_grenade_thrown     = 0;
    player->timers.since_last_shot               = 0;
    player->timers.time_since_last_wu            = 0;
    player->timers.since_last_weapon_input       = 0;
    player->timers.since_last_intel_tent_check   = 0;
    player->timers.since_last_water_damage       = 0;
    player->hp                                   = 100;
    player->blocks                               = 50;
    player->grenades                             = 3;
    if (reset == 0) {
        player->klass = GAME_CLASS_COMMANDO; /* a map change keeps the class */
    }
    player->class_speed                          = 1.0f;
    player->class_sprint                         = 0.0f;
    player->class_water                          = 0.0f;
    player->jump_velocity                        = -0.36f;
    player->fall_dist                            = 0.0f;
    player->has_intel                            = 0;
    player->squad                                = 0;
    player->squad_team                           = TEAM_SPECTATOR;
    player->hurt_at                              = 0;
    player->reloading                            = 0;
    player->client                               = ' ';
    player->version_minor                        = 0;
    player->version_major                        = 0;
    player->version_revision                     = 0;
    player->periodic_delay_index                 = 0;
    player->current_periodic_message             = server->periodic_messages;
    player->welcome_sent                         = 0;
    player->next_shot_invalid                    = 0;
    if (reset == 0) {
        player->permissions = 0;
    } else if (reset == 1) {
        _plr_free_grenades(server, player);
    }
    player->is_invisible = 0;
    player->alt_weapon   = 255; /* no second gun (Classes.c) */
    player->gun_slot     = 0;   /* the primary back in hand (Classes.c) */
    player->flying       = 0;
    player->fly_up       = 0;
    player->p10_input_prev = 0;
    player->move_scale     = 1.0f;
    player->kills        = 0;
    player->deaths       = 0;
    player->assists      = 0;
    player->score        = 0;
    player->captures     = 0;
    player->shots        = 0;
    player->hits         = 0;
    player->blocks_placed = 0;
    player->flank_stage  = 0;
    player->flank_x      = 0.0f;
    player->flank_y      = 0.0f;
    /* a map start takes everyone off whatever they sat on (Mount.c) */
    player->mount_entity  = P10_NO_OWNER;
    player->mount_seat    = 0;
    player->mount_vehicle = 0;
    /* a map reset keeps the player (and its name): humans re-send "join" and
       bots never get a new name, so clearing it here left bots nameless and
       their kills unnamed in the feed after the next map */
    if (reset == 0) {
        memset(player->name, 0, PLAYER_NAME_STRLEN + 1);
    }
    memset(player->os_info, 0, 255);
}

/* The spawn itself: 1.0 clients learn of it from the bridge's per-tick diff
   (Proto10/Bridge.c), which picks up STATE_READY. A player that was never
   gone (the arena's countdown brings the living back behind the gate) is no
   diff: for_players runs in _world_update, before proto10_bridge_tick, so
   the SPAWNING state is over by the bridge's next look and no Create went
   out. The client then kept its old tool, grenades and ammo while the
   server had the fresh gun. The body is always sent again, as a death's
   respawn already did. */
void send_respawn(server_t* server, player_t* respawn_player)
{
    proto10_bridge_light_off(respawn_player, "spawn");
    respawn_player->state = STATE_READY;
    proto10_bridge_body_recreate(respawn_player);
    /* 0.75 clients learn a spawn from CreatePlayer (the spawner's own too) */
    if (profile_is_075()) {
        legacy_send_respawn(server, respawn_player);
    }
}

/* A spawn is a fresh life: health, inventory, ammo and movement reset. */
static void _plr_spawn_reset(server_t* server, player_t* player)
{
    player->hp             = 100;
    player->fall_dist      = 0.0f; /* a new life, a new drop */
    player->grenades       = 3;
    player->blocks         = 50;
    player->item           = TOOL_GUN;
    player->input          = 0;
    player->move_forward   = 0;
    player->move_backwards = 0;
    player->move_left      = 0;
    player->move_right     = 0;
    player->jumping        = 0;
    player->crouching      = 0;
    player->sneaking       = 0;
    player->sprinting      = 0;
    player->primary_fire   = 0;
    player->secondary_fire = 0;
    player->alive          = 1;
    player->reloading      = 0;
    /* a spawn is a fresh life: the gun in hand full. A death's respawn had
       its ammo put back by the KillAction, but the arena's countdown brings
       the living back without one, and a player who survived a round kept a
       drained gun (the client's Create says the magazine is full).
       classes_on_spawn sets the class's own gun and clip after this */
    set_default_player_ammo(player);
    set_default_player_pellets(player);
    player->flank_stage    = 0; /* a new life, a new flank */
    /* the team's colour is the blocks' default at every spawn (the client
       does the same); a bot's wall work still paints with the ground's
       colour */
    player->tool_color = server->protocol.color_team[player->team];
    classes_on_spawn(server, player);
    /* a mode that carries no blocks (the arena): nothing in the inventory
       (classes_on_spawn returns early without classes) */
    if (gamemode_player_rules(server, player) & GM_RULE_NO_BLOCK) {
        player->blocks = 0;
    }
}

void on_player_update(server_t* server, player_t* player)
{
    if (player->protocol == PROTOCOL_10) {
        proto10_update(server, player);
        if (proto10_world_player_update(server, player)) {
            return; /* the map was reset: downloading it again */
        }
        /* after "join" a 1.0 player goes through the same spawn and respawn
           states as everyone else; only its packets differ */
        if (player->state == STATE_PROTO10) {
            return;
        }
    }
    /* a connected 0.75 client: the map download and the team screen are the
       0.75 wire's (Legacy/Join075.c); from SPAWNING on, the same as anyone */
    if (legacy_join_update(server, player)) {
        return;
    }
    switch (player->state) {
        case STATE_DISCONNECTED:
            break;
        /* Only bots reach STATE_STARTING_MAP and STATE_JOINING: a 1.0 human is
           moved back to STATE_PROTO10 by proto10_world_player_update above
           (map reset) and joins through "join" (game_player_join). A bot has
           no client to download the map, so it goes straight to picking a
           team. */
        case STATE_STARTING_MAP:
            player->state = STATE_JOINING;
            break;
        case STATE_JOINING:
            if (player->is_bot) {
                /* A human leaves this state by picking a team in the client
                   (receive_short_player). A bot has no client, so act on its
                   behalf: choosing the team is what moves it to STATE_SPAWNING
                   and makes the server spawn and announce it. */
                if (player->team != TEAM_A && player->team != TEAM_B) {
                    player->team = (server->protocol.num_team_users[TEAM_A]
                                    <= server->protocol.num_team_users[TEAM_B])
                                     ? TEAM_A
                                     : TEAM_B;
                }
                uint8_t wanted = player->team;
                if (plugins_on_join(server, player, &wanted) && wanted != TEAM_SPECTATOR) {
                    player->team = wanted;
                }
                set_default_player_ammo(player);
                player->state = STATE_SPAWNING;
                LOG_INFO("Bot %s (#%hhu) picked team %hhu", player->name, player->id, player->team);
            }
            break;
        case STATE_SPAWNING:
            if (player->team != TEAM_SPECTATOR) {
                _plr_spawn_reset(server, player);
            }
            set_player_respawn_point(server, player);
            player_face_objective(server, player);
            jetpack_on_spawn(server, player);
            spawnprotect_on_spawn(player);
            send_respawn(server, player);
            /* after the Create: the client sets its own spawn figure from it,
               and the wallet settles a plugin's or a mode's own */
            proto10_bridge_wallet(server, player);
            /* autofill makes room for humans. Run on a bot's spawn, it freed
               bots (even the one spawning) in the middle of this tick: the
               log then showed a nameless player #0 spawning at 0,0,0 */
            if (!player->is_bot) {
                bots_on_player_join(server);
            }
            LOG_INFO("Player %s (#%hhu) spawning at: %f %f %f",
                     player->name,
                     player->id,
                     player->movement.position.x,
                     player->movement.position.y,
                     player->movement.position.z);
            evt_spawn(player);
            if (player->team != TEAM_SPECTATOR) {
                plugins_on_spawn(server, player);
            }
            break;
        case STATE_WAITING_FOR_RESPAWN:
        {
            /* in an arena round the dead wait for the next round, which
               respawns everyone at once (the Arena mode's tick) */
            if (get_game_seconds() - player->timers.start_of_respawn_wait >= player->respawn_time &&
                gamemode_allows_respawn(server, player))
            {
                player->state = STATE_SPAWNING;
            }
            break;
        }
        case STATE_READY:
            break;
        default:
            /* disconnected */
            break;
    }
}

void player_address(const player_t* player, char* out, size_t size)
{
    if (player->conn != NULL && player->conn->address[0] != '\0') {
        snprintf(out, size, "%s", player->conn->address);
        return;
    }
    char ip[19];
    format_ip_to_str(ip, player->ip);
    snprintf(out, size, "%s", ip);
}

/* Bans.json, its "Bans" array always there: a missing file (a new data
   folder: the console's or MCP's first ban before anyone joined), a file
   that is not a JSON object or one without the array left the callers'
   array pointer unset, and the first ban crashed the server. The caller
   frees the root (json_object_put) */
struct json_object* bans_open(struct json_object** array)
{
    struct json_object* root = json_object_from_file(data_bans_file());
    if (root != NULL && !json_object_is_type(root, json_type_object)) {
        json_object_put(root);
        root = NULL;
    }
    if (root == NULL) {
        root = json_object_new_object();
    }
    if (!json_object_object_get_ex(root, "Bans", array) || !json_object_is_type(*array, json_type_array)) {
        *array = json_object_new_array();
        json_object_object_add(root, "Bans", *array);
    }
    return root;
}

int player_banned(const conn_t* conn)
{
    const uint8_t* ip4 = conn->ip4;
    char           prefix[64];
    int            v6 = conn->family == 6 && conn_ipv6_prefix(conn->address, prefix, sizeof prefix);
    uint8_t             banned_user = 0;
    struct json_object* array;
    struct json_object* root        = bans_open(&array);
    /* the file is made at the first connection, as it always was */
    if (access(data_bans_file(), F_OK) != 0) {
        json_object_to_file(data_bans_file(), root);
    }
    ip_t hostIP;
    hostIP.cidr  = 24;
    hostIP.ip[0] = ip4[0];
    hostIP.ip[1] = ip4[1];
    hostIP.ip[2] = ip4[2];
    hostIP.ip[3] = ip4[3];
    int count = json_object_array_length(array);
    for (int i = 0; i < count; ++i) {
        struct json_object* object_at_index = json_object_array_get_idx(array, i);
        const char*         IP;
        const char*         start_of_range_string;
        const char*         end_of_range_string;
        READ_STR_FROM_JSON(object_at_index, start_of_range_string, start_of_range, "start of range", "0.0.0.0", 1);
        READ_STR_FROM_JSON(object_at_index, end_of_range_string, end_of_range, "end of range", "0.0.0.0", 1);
        READ_STR_FROM_JSON(object_at_index, IP, IP, "IP", "0.0.0.0", 1);
        ip_t ipStruct;
        ip_t start_of_range, end_of_range;
        /* an IPv6 ban's "IP" is its /64, which the IPv4 parse refuses */
        int matched = 0;
        if (v6) {
            matched = strcmp(IP, prefix) == 0;
        } else if (conn->family == 4 && format_str_to_ip((char*) IP, &ipStruct) &&
                   format_str_to_ip((char*) start_of_range_string, &start_of_range) &&
                   format_str_to_ip((char*) end_of_range_string, &end_of_range))
        {
            matched = ip_in_range(hostIP, ipStruct, start_of_range, end_of_range);
        }
        if (matched) {
            const char* name_of_player;
            const char* reason;
            double      time    = 0.0f;
            uint64_t    timeNow = get_nanos();
            READ_STR_FROM_JSON(object_at_index, name_of_player, Name, "Name", "Deuce", 0);
            READ_STR_FROM_JSON(object_at_index, reason, Reason, "Reason", "None", 0);
            READ_DOUBLE_FROM_JSON(object_at_index, time, Time, "Time", 0.0, 0);
            if (((long double) timeNow / NANO_IN_MINUTE) > time && time != 0) {
                json_object_array_del_idx(array, i, 1);
                json_object_to_file(data_bans_file(), root);
                /* delete every expired ban that matches, not just the first */
                continue;
            }
            LOG_WARNING("Banned user %s tried to join with IP: %s Banned for: %s",
                        name_of_player,
                        conn->address,
                        reason);
            banned_user = 1;
            break;
        }
    }
    json_object_put(root);
    return banned_user;
}

/* Connections that have not entered the game yet (a 1.0 handshake or map
   download, a 0.75 one before its team choice) hold a player slot each;
   forged connects that never finish them could fill the server. They get a
   cap of their own, under max_players, so the players already in keep
   their room; at the cap a new connection pushes out the oldest one still
   in the first handshake steps, which a real client leaves in
   milliseconds, so a real joiner gets in while such a flood goes on */
static int _plr_half_open_cap(server_t* server)
{
    int cap = server->protocol.max_players / 4;
    return cap < 4 ? 4 : cap;
}

static int _plr_half_open(server_t* server)
{
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot || p->conn == NULL) {
            continue;
        }
        if (p->protocol == PROTOCOL_10 ? p->p10_state != P10_STATE_GAME && p->p10_state != P10_STATE_CLOSING
                                       : !p->welcome_sent)
        {
            n++;
        }
    }
    return n;
}

/* the oldest connection still in the first handshake steps, NULL if none */
static player_t* _plr_half_open_oldest(server_t* server)
{
    player_t *p, *tmp, *oldest = NULL;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->conn != NULL && p->protocol == PROTOCOL_10 && proto10_in_handshake(p) &&
            (oldest == NULL || p->p10_deadline < oldest->p10_deadline))
        {
            oldest = p;
        }
    }
    return oldest;
}

void on_new_player_connection(server_t* server, conn_t* conn, uint32_t connect_data)
{
    /* 0xFF until a player id is given: a refused connection's disconnect
       must not reach a player. The mark was max_players - 1, a real id (31,
       now 63), and a NULL peer data read as player 0 */
    conn->player_id = CONN_NO_PLAYER;
    uint8_t player_id;
    uint8_t protocol;
    /* this server speaks protocol 1.0 only (CONTRIBUTING.md, "Protocols"): our client
       speaks it, vanilla clients get "wrong protocol" exactly as a vanilla
       server answers a version it does not know */
    if (profile_is_075()) {
        /* the 0.75 profile (docs/plan-075-server.md): 0.75 and 0.76 only,
           and our client's 1.0 attempt gets the same "wrong protocol" a
           vanilla server answers, so it falls back to 0.75 */
        if (connect_data != VERSION_0_75 && connect_data != VERSION_0_76) {
            LOG_INFO("Refused a connection with connect data %u: this server speaks protocol 0.75 only",
                     (unsigned) connect_data);
            conn_disconnect(conn, REASON_WRONG_PROTOCOL_VERSION, CONN_CLOSE_NOW);
            return;
        }
        protocol = PROTOCOL_075;
    } else if (connect_data != P10_CONNECT_MAGIC) {
        LOG_INFO("Refused a connection with connect data %u: this server speaks protocol 1.0 only",
                 (unsigned) connect_data);
        conn_disconnect(conn, REASON_WRONG_PROTOCOL_VERSION, CONN_CLOSE_NOW);
        return;
    } else {
        protocol = PROTOCOL_10;
    }

    if (player_banned(conn)) {
        conn_disconnect(conn, REASON_BANNED, CONN_CLOSE);
        return;
    }
    if (_plr_half_open(server) >= _plr_half_open_cap(server)) {
        player_t* oldest = _plr_half_open_oldest(server);
        if (oldest == NULL) {
            evt_log(NULL, "net", "action=refused why=half_open cap=%d address=%s", _plr_half_open_cap(server),
                    conn->address);
            conn_disconnect(conn, REASON_SERVER_FULL, CONN_CLOSE_NOW);
            return;
        }
        evt_log(oldest, "net", "action=drop why=half_open_oldest cap=%d", _plr_half_open_cap(server));
        proto10_evict(oldest, "half-open cap");
    }
    player_id = _plr_next_free_id(server);
    if (player_id == 0xFF) {
        conn_disconnect(conn, REASON_SERVER_FULL, CONN_CLOSE_NOW);
        LOG_WARNING("Server full. Kicking player");
        return;
    }
    player_t*  player  = spadesx_calloc(1, sizeof(player_t));
    vector3f_t empty   = {0, 0, 0};
    vector3f_t forward = {1, 0, 0};
    vector3f_t height  = {0, 0, 1};
    vector3f_t strafe  = {0, 1, 0};
    init_player(server, player, 0, 0, empty, forward, strafe, height);
    player->id        = player_id;
    player->conn      = conn;
    conn->player_id   = player_id;
    player->hp        = 100;
    player->ip.ip[0]  = conn->ip4[0];
    player->ip.ip[1]  = conn->ip4[1];
    player->ip.ip[2]  = conn->ip4[2];
    player->ip.ip[3]  = conn->ip4[3];

    snprintf(player->name, 6, "Limbo");
    LOG_INFO("Player %s (%s, #%hhu) connected", player->name, conn->address, player_id);

    player->timers.since_periodic_message = get_nanos();
    player->current_periodic_message      = server->periodic_messages;
    player->periodic_delay_index          = 0;
    player->protocol                      = protocol;
    player->state                         = protocol == PROTOCOL_10 ? STATE_PROTO10 : STATE_STARTING_MAP;
    HASH_ADD(hh, server->players, id, sizeof(uint8_t), player);
    HASH_SORT(server->players, player_sort);
    /* a listen server's own player is its host: it enters with every role,
       not only the command bypass player_has_permission gives it, so its
       scoreboard says admin and /login answers "already logged in" */
    if (server->listen_mode && player_is_host(server, player)) {
        player->permissions |= 0x1Fu;
        evt_log(player, "host", "action=admin permissions=%u", player->permissions);
    }
    if (protocol == PROTOCOL_10) {
        LOG_INFO("Player #%hhu speaks protocol 1.0", player_id);
        proto10_begin(server, player);
    } else {
        legacy_on_connect(server, player, connect_data);
    }
}

/* The reload, when the player is not firing and has the gun out: a magazine
   gun (rifle, SMG), a per shell shotgun, and the rocket/revolver's fixed
   clip. Each runs on its own timer. */
static void _plr_reload_tick(server_t* server, player_t* player, uint64_t timeNow)
{
    switch ((int) player->weapon) {
        case GAME_WEAPON_ROCKET:
        case GAME_WEAPON_REVOLVER:
        {
            uint64_t ms = player->weapon == GAME_WEAPON_ROCKET ? ROCKET_RELOAD_MS : GAME_REVOLVER_RELOAD_MS;
            if (diff_is_older_then(timeNow, &player->timers.since_reload_start, NANO_IN_MILLI * ms)) {
                uint8_t take = (uint8_t) (player->default_clip - player->weapon_clip);
                if (take > player->weapon_reserve) {
                    take = (uint8_t) player->weapon_reserve;
                }
                player->weapon_clip += take;
                player->weapon_reserve -= take;
                player->reloading = 0;
            }
            break;
        }
        case WEAPON_RIFLE:
        case WEAPON_SMG:
        {
            if (diff_is_older_then(
                timeNow, &player->timers.since_reload_start,
                NANO_IN_MILLI * (uint64_t) (weapon_is_sniper(player) ? GAME_SNIPER_RELOAD_MS : 2500)))
            {
                uint8_t defaultAmmo = weapon_is_sniper(player) ? GAME_SNIPER_CLIP : RIFLE_DEFAULT_CLIP;
                if (player->weapon == WEAPON_SMG) {
                    defaultAmmo = SMG_DEFAULT_CLIP;
                }
                double newReserve = fmax(0, player->weapon_reserve - (defaultAmmo - player->weapon_clip));
                player->weapon_clip += player->weapon_reserve - newReserve;
                player->weapon_reserve = newReserve;
                player->reloading      = 0;
                if (profile_is_075()) {
                    legacy_send_weapon_reload(server, player, 0);
                }
            }
            break;
        }
        case WEAPON_SHOTGUN:
        {
            if (diff_is_older_then(
                timeNow, &player->timers.since_reload_start, NANO_IN_MILLI * (uint64_t) 500))
            {
                if (player->weapon_reserve == 0 || player->weapon_clip == SHOTGUN_DEFAULT_CLIP) {
                    player->reloading = 0;
                    break;
                }
                player->weapon_clip++;
                player->weapon_reserve--;
                if (player->weapon_clip == SHOTGUN_DEFAULT_CLIP) {
                    player->reloading = 0;
                }
                if (profile_is_075()) {
                    legacy_send_weapon_reload(server, player, 0);
                }
            }
            break;
        }
    }
}

/* Firing and reloading, once a player is past the join screen. */
static void _plr_weapon_tick(server_t* server, player_t* player)
{
    uint64_t timeNow = get_nanos();

    player->timers.since_previous_shot = player->timers.since_last_shot;
    uint64_t previous_shot_delay       = timeNow - player->timers.since_previous_shot;
    uint64_t original_weapon_delay     = get_player_weapon_delay_nano(player);
    /* The next shot waits the official delay minus 3 ms. A shot that came
       sooner shortens the wait by how soon it came, so a slightly fast
       shooter falls back in line; one over twice the official delay gets no
       wait at all. */
    uint64_t weapon_delay = (previous_shot_delay <= original_weapon_delay)
                              ? (original_weapon_delay - 3000000)
                              : (original_weapon_delay - 3000000 -
                                 (previous_shot_delay < (original_weapon_delay * 2)
                                      ? (previous_shot_delay - original_weapon_delay)
                                      : original_weapon_delay - 3000001));

    if (diff_is_older_then_dont_update(timeNow, player->timers.since_last_weapon_input, weapon_delay)) {
        set_default_player_pellets(player);
    }

    /* a rocket is spent by the rocket command (Proto10/Rockets.c),
       not by holding the trigger */
    if (player->mount_entity == 0 && player->primary_fire == 1 && player->reloading == 0 &&
        player->item == TOOL_GUN && player->weapon != GAME_WEAPON_ROCKET)
    {
        if (player->weapon_clip > 0) {
            if (diff_is_older_then(
                timeNow, &player->timers.since_last_weapon_input, get_player_weapon_delay_nano(player)))
            {
                player->weapon_clip--;
                if (player->weapon_clip == 0) {
                    player->primary_fire = 0;
                    if (profile_is_075()) {
                        legacy_send_weapon_input(server, player, 0);
                    }
                }
            }
        }
    } else if (player->primary_fire == 0 && player->reloading == 1 && player->item == TOOL_GUN) {
        _plr_reload_tick(server, player, timeNow);
    }

    if (diff_is_older_then(timeNow,
                           &player->timers.since_periodic_message,
                           (uint64_t) (server->periodic_delays[player->periodic_delay_index] * 60) *
                           NANO_IN_SECOND))
    {
        string_node_t* message;
        DL_FOREACH(server->periodic_messages, message)
        {
            send_server_notice(player, 0, message->string);
        }
        player->periodic_delay_index = fmin(player->periodic_delay_index + 1, 4);
    }
}

/* The once a second mode hooks for a ready player. */
static void _plr_mode_tick(server_t* server, player_t* player)
{
    gamemode_player_step(server, player);
    uint64_t time_now = get_nanos();
    if (diff_is_older_then(time_now, &player->timers.since_last_intel_tent_check, NANO_IN_SECOND)) {
        gamemode_player_tick(server, player);
        /* a map rule, whatever the mode: boundary_damage */
        map_meta_player_tick(server, player);
        plugins_on_player_tick(server, player);
    }
}

void for_players(server_t* server)
{
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        lagcomp_record(server, player);
        conduct_player_tick(server, player);
        if (is_past_join_screen(player)) {
            _plr_weapon_tick(server, player);
        }
        if (player->state == STATE_READY) {
            _plr_mode_tick(server, player);
        }
    }
}
