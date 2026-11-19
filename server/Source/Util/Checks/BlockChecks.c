#include "Util/Checks/PositionChecks.h"
#include <Server/MapStore.h>

#include <Server/Classes.h>
#include <Server/Staff.h>
#include <Server/Structs/CommandStruct.h>
#include <Server/Structs/PlayerStruct.h>
#include <Util/Checks/BlockChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Enums.h>
#include <Util/Weapon.h>
#include <classes/classes.h>
#include <Util/Log.h>
#include <stdint.h>

/* the slack every delay check grants a client whose packets arrive a little
   closer together than the rule allows */
#define MICRO_ALLOWANCE NANO_IN_MILLI * 5

uint8_t is_block_placable(server_t* server, vector3i_t pos)
{
    /* the map's last two layers are the water's, where nothing is built */
    if (pos.z >= server->s_map.map.size_z - 2) {
        return 0;
    }

    /* a block needs something to hold it: any of the six neighbours will do */
    static const vector3i_t neighbours[6] = {{0, 0, -1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    for (int i = 0; i < 6; i++) {
        int x = pos.x + neighbours[i].x;
        int y = pos.y + neighbours[i].y;
        int z = pos.z + neighbours[i].z;
        if (valid_pos_3i(server, x, y, z) && map_solid(&server->s_map.map, x, y, z)) {
            return 1;
        }
    }
    return 0;
}

/* how long a gun must rest between two blocks it destroys, 0 for a weapon
   that breaks no block */
static uint64_t _block_gun_delay(player_t* player, uint8_t weapon)
{
    switch (weapon) {
        case WEAPON_RIFLE:
            return weapon_is_sniper(player) ? NANO_IN_MILLI * GAME_SNIPER_DELAY_MS : RIFLE_DELAY;
        case WEAPON_SMG:
            return SMG_DELAY;
        case WEAPON_SHOTGUN:
            return SHOTGUN_DELAY;
        default:
            return 0;
    }
}

/* A gun that destroys a block and switches tools in the same breath is one
   of the two ways a client gets round the gun's own rate check: the tool is
   no longer the gun when that check runs. */
static uint8_t _destroyed_with_gun_too_fast(player_t* player, uint64_t time_now)
{
    uint64_t delay = _block_gun_delay(player, player->weapon);
    if (delay == 0) {
        return 0;
    }
    uint64_t since_last_shot = player->timers.since_last_block_dest_with_gun;

    return !diff_is_older_then_dont_update(time_now, since_last_shot, delay - MICRO_ALLOWANCE);
}

/* Whether the timers allow the action. The action's own timer comes first and
   is the one the check updates, the other two are only read: the order is the
   one the checks have always had, and a refused action leaves the timers as
   they were. */
static uint8_t _action_delay_ok(server_t* server, player_t* player, uint64_t time_now, uint8_t action_type)
{
    uint64_t  delay;
    uint64_t* own;
    uint64_t* other[2];

    switch (action_type) {
        case BLOCKACTION_BUILD:
            delay    = BLOCK_DELAY;
            own      = &player->timers.since_last_block_plac;
            other[0] = &player->timers.since_last_block_dest;
            other[1] = &player->timers.since_last_3block_dest;
            break;
        case BLOCKACTION_DESTROY_ONE:
            delay    = SPADE_DELAY;
            own      = &player->timers.since_last_block_dest;
            other[0] = &player->timers.since_last_3block_dest;
            other[1] = &player->timers.since_last_block_plac;
            break;
        case BLOCKACTION_DESTROY_THREE:
            /* the class's pace (the Miner's is quicker), THREEBLOCK_DELAY
               without classes */
            delay    = classes_dig_delay(server, player);
            own      = &player->timers.since_last_3block_dest;
            other[0] = &player->timers.since_last_block_dest;
            other[1] = &player->timers.since_last_block_plac;
            break;
        default:
            return 0;
    }

    uint64_t limit = delay - MICRO_ALLOWANCE;
    return diff_is_older_then(time_now, own, limit) && diff_is_older_then_dont_update(time_now, *other[0], limit) &&
           diff_is_older_then_dont_update(time_now, *other[1], limit);
}

uint8_t block_action_delay_check(server_t* server,
                                 player_t* player,
                                 uint64_t  time_now,
                                 uint8_t   action_type,
                                 uint8_t   ignore_weapon)
{
    if (ignore_weapon == 0 && _destroyed_with_gun_too_fast(player, time_now)) {
        send_message_to_staff(server,
                              "Player %s (#%hhu) tried to switch tools and destroy blocks faster then allowed",
                              player->name,
                              player->id);
        return 0;
    }

    if (_action_delay_ok(server, player, time_now, action_type)) {
        return 1;
    }

    send_message_to_staff(server, "Player %s (#%hhu) probably has speed destroy hack", player->name, player->id);
    return 0;
}
