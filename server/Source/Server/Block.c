#include <Server/Plugins.h>
#include "Util/Log.h"
#include <Server/EventLog.h>

#include <Server/Block.h>
#include <Server/BlockHealth.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Mount.h>
#include <stdio.h>
#include <Server/Classes.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Nodes.h>
#include <Server/Proto10/World.h>
#include <Server/Packets/Packets.h>
#include <Server/Staff.h>
#include <Util/Checks/BlockChecks.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Checks/WeaponChecks.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>

static void
block_action_build(server_t* server, player_t* player, uint8_t action_type, uint32_t X, uint32_t Y, uint32_t Z)
{
    uint64_t time_now = get_nanos();
    vector3i_t position = {X, Y, Z};
    if (gamemode_player_rules(server, player) & GM_RULE_NO_BLOCK) {
        evt_log(player, "block", "action=build cell=%d,%d,%d accepted=0 why=mode", (int) X, (int) Y, (int) Z);
        return;
    }
    if (!(gamemode_cell_editable(server, X, Y, Z) && player->blocks > 0 &&
        block_action_delay_check(server, player, time_now, action_type, 1) && is_block_placable(server, position)))
    {
        evt_log(player, "block", "action=build cell=%d,%d,%d accepted=0 why=%s blocks_left=%u bot=%u item=%u",
                (int) X, (int) Y, (int) Z,
                !gamemode_cell_editable(server, X, Y, Z) ? gamemode_cell_why(server, X, Y, Z)
                                                        : (player->blocks == 0
                                                               ? "no-blocks"
                                                               : (!is_block_placable(server, position) ? "not-placeable"
                                                                                                       : "rate-limited")),
                player->blocks, player->is_bot, player->item);
        return;
    }
    if (!plugins_on_build(server, player, (int) X, (int) Y, (int) Z)) {
        return;
    }
    map_set_color(&server->s_map.map, X, Y, Z, player->tool_color.raw);
    proto10_world_cell(server, X, Y, Z, P10_WORLD_CAUSE_PLAYER);
    player->blocks--;
    player->blocks_placed++;
    proto10_bridge_wallet(server, player);
    evt_block(player, "build", (int) X, (int) Y, (int) Z, 1, player->blocks);
    gamemode_terrain_changed(server, 1);
}

/* Several block hits of one shot. On protocol 1.0 the client sends every
   pellet that meets a block (block health, BlockHealth.c): a shotgun's
   shot is up to its 8 pellets. On the 0.75 profile the client sends only
   the hit that breaks, and a class that breaks blocks faster (block_damage)
   may break up to classes_gun_breaks blocks a shot. Either way the hits of
   one shot leave in its frame and arrive in one tick: the first goes
   through the gun's checks (the fire rate, the clip, the spade's timers),
   which start the window; the next ones within GUN_SHOT_WINDOW of it are
   that same shot, counted, never taken for a rapid fire hack (the checks
   would refuse them and tell the staff). The cell rules (mode, map,
   protected cells, the water layer) apply to each. */
#define GUN_SHOT_WINDOW (NANO_IN_MILLI * 100)

static int block_action_gun_ok(server_t* server, player_t* player, uint64_t time_now)
{
    int p10      = !profile_is_075();
    int per_shot = p10 ? (player->weapon_pellets > 0 ? player->weapon_pellets : 1) : classes_gun_breaks(server, player);
    if (per_shot > 1 && player->gun_hits > 0 && player->gun_hits < per_shot &&
        time_now - player->timers.since_last_block_dest_with_gun < GUN_SHOT_WINDOW)
    {
        player->gun_hits++;
        return 1;
    }
    if (!player_gun_raised(player, time_now)) {
        return 0;
    }
    if (!(p10 ? block_action_weapon_checks_paced(server, player, time_now)
              : block_action_weapon_checks(server, player, time_now)))
    {
        evt_log(player, "block", "action=gun_hit accepted=0 why=rate weapon=%u", (unsigned) player->weapon);
        return 0;
    }
    player->gun_hits = 1;
    return 1;
}

/* Protocol 1.0: the gun's hit takes the block's health. Returns 1 when it
   would break it (the caller asks the plugins and breaks it, and
   proto10_world_cell forgets the cell); 0 when the hit only cracked it,
   taken here and queued as a TerrainUpdate edit (proto10_world_cell), so
   everyone draws the crack, not only the shooter. The EVT
   line is one a hit: the event log is off on a public server, and in a
   test each hit is what is checked. */
static int block_action_gun_cracks(server_t* server, player_t* player, uint32_t X, uint32_t Y, uint32_t Z)
{
    int damage = classes_block_damage(server, player);
    int taken  = block_health_peek(server, (int) X, (int) Y, (int) Z, damage);
    evt_log(player, "block_damage", "cell=%d,%d,%d hp=%d damage=%d by=%u", (int) X, (int) Y, (int) Z,
            GAME_BLOCK_BREAK - taken, damage, (unsigned) player->weapon);
    if (taken >= GAME_BLOCK_BREAK) {
        return 1;
    }
    block_health_hit(server, (int) X, (int) Y, (int) Z, damage);
    /* a NONE edit of a solid cell keeps the crack and carries the cell into
       the tick's TerrainUpdate, the health byte showing the damage taken */
    proto10_world_cell(server, (int) X, (int) Y, (int) Z, P10_WORLD_CAUSE_NONE);
    return 0;
}

static void
block_action_destroy_one(server_t* server, player_t* player, uint8_t action_type, uint32_t X, uint32_t Y, uint32_t Z)
{
    uint64_t time_now = get_nanos();
    if (gamemode_player_rules(server, player) & GM_RULE_NO_DESTROY) {
        evt_log(player, "block", "action=destroy cell=%d,%d,%d accepted=0 why=mode", (int) X, (int) Y, (int) Z);
        return;
    }
    if (map_meta_cell_protected(server, X, Y, Z)) {
        evt_log(player, "block", "action=destroy cell=%d,%d,%d accepted=0 why=map-protected", (int) X, (int) Y, (int) Z);
        return;
    }
    if (!((Z < 62 && gamemode_cell_editable(server, X, Y, Z)) &&
          ((player->item == TOOL_SPADE && block_action_delay_check(server, player, time_now, action_type, 0)) ||
          (player->item == TOOL_GUN && block_action_gun_ok(server, player, time_now)))))
    {
        return;
    }
    /* a gun's hit that only cracks the block is no destroy: the plugins'
       on_destroy hears the one that breaks it, as before block health */
    if (player->item == TOOL_GUN && !profile_is_075() && !block_action_gun_cracks(server, player, X, Y, Z)) {
        return;
    }
    if (!plugins_on_destroy(server, player, (int) X, (int) Y, (int) Z, player->item == TOOL_GUN ? "gun" : "spade")) {
        return;
    }

    vector3i_t  position = {X, Y, Z};
    vector3i_t* neigh    = get_neighbours(position);
    map_set_air(&server->s_map.map, position.x, position.y, position.z);
    proto10_world_cell(server, position.x, position.y, position.z, P10_WORLD_CAUSE_PLAYER);
    for (int i = 0; i < 6; ++i) {
        if (neigh[i].z < 62) {
            check_node(server, neigh[i]);
        }
    }
    if (player->item == TOOL_GUN && classes_gun_breaks(server, player) > 1) {
        evt_log(player, "class", "action=gun_break cell=%d,%d,%d weapon=%u shot_block=%u of=%d", (int) X, (int) Y,
                (int) Z, (unsigned) player->weapon, (unsigned) player->gun_hits, classes_gun_breaks(server, player));
    }
    if (player->item != TOOL_GUN) {
        if (player->blocks < classes_blocks_max(server, player)) {
            player->blocks++;
            proto10_bridge_wallet(server, player);
        }
    }
}

static void
block_action_destroy_three(server_t* server, player_t* player, uint8_t action_type, uint32_t X, uint32_t Y, uint32_t Z)
{
    uint64_t time_now = get_nanos();
    if (gamemode_player_rules(server, player) & GM_RULE_NO_DESTROY) {
        evt_log(player, "block", "action=destroy3 cell=%d,%d,%d accepted=0 why=mode", (int) X, (int) Y, (int) Z);
        return;
    }
    if (map_meta_cell_protected(server, X, Y, Z) || map_meta_cell_protected(server, X, Y, Z + 1) ||
        map_meta_cell_protected(server, X, Y, Z - 1))
    {
        evt_log(player, "block", "action=destroy3 cell=%d,%d,%d accepted=0 why=map-protected", (int) X, (int) Y, (int) Z);
        return;
    }
    if (!(gamemode_cell_editable(server, X, Y, Z) && gamemode_cell_editable(server, X, Y, Z + 1) &&
        gamemode_cell_editable(server, X, Y, Z - 1) && block_action_delay_check(server, player, time_now, action_type, 1)) || player->item == TOOL_GUN)
    {
        return;
    }
    if (!plugins_on_destroy(server, player, (int) X, (int) Y, (int) Z, "dig")) {
        return;
    }
    /* the Miner's spade (a class dig of 3): the cells round the column too,
       a 3 x 3 x 3 cube, each one the map and the mode let go */
    int wide  = classes_dig(server, player) >= 3;
    int dug   = 0; /* the solid cells the dig cleared: a block comes back a cell */
    if (wide) {
        int cleared = 0;
        /* the cube's own cells, the column's counted below */
        for (int z = (int) Z - 1; z <= (int) Z + 1; z++) {
            for (int x = (int) X - 1; x <= (int) X + 1; x++) {
                for (int y = (int) Y - 1; y <= (int) Y + 1; y++) {
                    if ((x == (int) X && y == (int) Y) || !classes_cell_diggable(server, x, y, z)) {
                        continue;
                    }
                    map_set_air(&server->s_map.map, x, y, z);
                    proto10_world_cell(server, x, y, z, P10_WORLD_CAUSE_PLAYER);
                    cleared++;
                }
            }
        }
        evt_log(player, "class", "action=dig3 cell=%d,%d,%d cleared=%d", (int) X, (int) Y, (int) Z, cleared + 3);
        dug += cleared;
    }
    for (uint32_t z = Z - 1; z <= Z + 1; z++) {
        if (z >= 62) {
            continue;
        }
        int solid = map_solid(&server->s_map.map, X, Y, z);
        if (wide) {
            /* what hung on the cube falls: its shell is checked below */
            map_set_air(&server->s_map.map, X, Y, z);
            proto10_world_cell(server, X, Y, z, P10_WORLD_CAUSE_PLAYER);
            dug += solid;
            continue;
        }
        map_set_air(&server->s_map.map, X, Y, z);
        vector3i_t  position = {X, Y, z};
        vector3i_t* neigh    = get_neighbours(position);
        map_set_air(&server->s_map.map, position.x, position.y, position.z);
        proto10_world_cell(server, position.x, position.y, position.z, P10_WORLD_CAUSE_PLAYER);
        dug += solid;
        for (int i = 0; i < 6; ++i) {
            if (neigh[i].z < 62) {
                check_node(server, neigh[i]);
            }
        }
    }
    if (wide) {
        classes_check_shell(server, (int) X, (int) Y, (int) Z, 1);
    }
    /* the dig gives a block a cell back, up to the wallet's max, as the
       classic client always did for its swing (retail add_blocks) */
    int cap = classes_blocks_max(server, player);
    if (dug > 0 && player->blocks < cap) {
        int now = player->blocks + dug;
        player->blocks = (uint16_t) (now > cap ? cap : now);
        proto10_bridge_wallet(server, player);
    }
}

void handle_block_action(server_t*  server,
                         player_t*  player,
                         uint8_t    action_type,
                         vector3i_t vector_block,
                         vector3f_t vectorf_block,
                         vector3f_t player_vector,
                         uint32_t   X,
                         uint32_t   Y,
                         uint32_t   Z)
{
    if ((distance_in_3d(vectorf_block, player_vector) > 4 && player->item != TOOL_GUN) ||
        !valid_pos_v3i(server, vector_block))
    {
        return;
    }
    if (mount_mounted(player)) {
        /* a seat is no place to build or dig from (Mount.c, 12.8) */
        evt_log(player, "block", "action=refused why=mounted cell=%d,%d,%d", (int) X, (int) Y, (int) Z);
        return;
    }
    switch (action_type) {
        case BLOCKACTION_BUILD:
        {
            block_action_build(server, player, action_type, X, Y, Z);
        } break;

        case BLOCKACTION_DESTROY_ONE:
        {
            block_action_destroy_one(server, player, action_type, X, Y, Z);
        } break;

        case BLOCKACTION_DESTROY_THREE:
        {
            block_action_destroy_three(server, player, action_type, X, Y, Z);
        } break;
    }
}
