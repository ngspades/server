#include <Server/Staff.h>
#include <Server/Structs/CommandStruct.h>
#include <Server/Structs/PlayerStruct.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Checks/WeaponChecks.h>
#include <Util/Enums.h>
#include <Util/Weapon.h>
#include <classes/classes.h>
#include <stdint.h>

#define MICRO_ALLOWANCE NANO_IN_MILLI * 5

uint8_t block_action_weapon_checks(server_t* server, player_t* player, uint64_t time_now)
{
    if (player->weapon_clip == 0 &&
        !diff_is_older_then_dont_update(time_now, player->timers.since_last_primary_weapon_input, 10 * NANO_IN_MILLI))
    {
        send_message_to_staff(
        server, "Player %s (#%hhu) probably has hack to have more ammo", player->name, player->id);
        return 0;
    } else if (player->weapon == WEAPON_RIFLE && !diff_is_older_then(time_now,
                                                                    &player->timers.since_last_block_dest_with_gun,
                                                                    ((weapon_is_sniper(player) ? NANO_IN_MILLI * GAME_SNIPER_DELAY_MS : RIFLE_DELAY) - MICRO_ALLOWANCE)))
    {
        send_message_to_staff(server, "Player %s (#%hhu) probably has rapid shooting hack", player->name, player->id);
        return 0;
    } else if (player->weapon == WEAPON_SMG && !diff_is_older_then(time_now,
                                                                  &player->timers.since_last_block_dest_with_gun,
                                                                  (SMG_DELAY - MICRO_ALLOWANCE)))
    {
        send_message_to_staff(server, "Player %s (#%hhu) probably has rapid shooting hack", player->name, player->id);
        return 0;
    } else if (player->weapon == WEAPON_SHOTGUN && !diff_is_older_then(time_now,
                                                                      &player->timers.since_last_block_dest_with_gun,
                                                                      (SHOTGUN_DELAY - MICRO_ALLOWANCE)))
    {
        send_message_to_staff(server, "Player %s (#%hhu) probably has rapid shooting hack", player->name, player->id);
        return 0;
    }
    else if (!(diff_is_older_then_dont_update(time_now, player->timers.since_last_block_dest, SPADE_DELAY) &&
                  diff_is_older_then_dont_update(time_now, player->timers.since_last_3block_dest, SPADE_DELAY) &&
                  diff_is_older_then_dont_update(time_now, player->timers.since_last_block_plac, SPADE_DELAY)))
    {
        send_message_to_staff(server, "Player %s (#%hhu) tried to switch tools and destroy blocks faster then allowed", player->name, player->id);
        return 0;
    }
    return 1;
}

/* Protocol 1.0 sends the server every shot that meets a block (block
   health), not only the one that breaks it: an SMG at a wall is a hit
   every 100 ms, and two shots the network bunched up arrived closer than
   the gun's delay, refused and reported to the staff as a rapid fire hack.
   So the rate is a schedule: each hit makes the next due one delay after
   this one was (or after now less the slack, when the player paused),
   and a hit is taken up to GUN_HIT_SLACK before it is due. A late shot
   does not earn the next one early beyond that slack, so a client firing
   faster than the gun falls behind its schedule and is refused. */
#define GUN_HIT_SLACK (NANO_IN_MILLI * 50)

static uint64_t _gun_block_delay(player_t* player, uint8_t weapon)
{
    switch (weapon) {
        case WEAPON_RIFLE:
            return weapon_is_sniper(player) ? NANO_IN_MILLI * GAME_SNIPER_DELAY_MS : RIFLE_DELAY;
        case WEAPON_SMG:
            return SMG_DELAY;
        case WEAPON_SHOTGUN:
            return SHOTGUN_DELAY;
        case GAME_WEAPON_REVOLVER:
            return (uint64_t) GAME_REVOLVER_DELAY_MS * NANO_IN_MILLI;
        default:
            return 0;
    }
}

uint8_t block_action_weapon_checks_paced(server_t* server, player_t* player, uint64_t time_now)
{
    uint64_t delay = _gun_block_delay(player, player->weapon);
    if (player->weapon_clip == 0 &&
        !diff_is_older_then_dont_update(time_now, player->timers.since_last_primary_weapon_input, 10 * NANO_IN_MILLI))
    {
        send_message_to_staff(
        server, "Player %s (#%hhu) probably has hack to have more ammo", player->name, player->id);
        return 0;
    }
    if (time_now + GUN_HIT_SLACK < player->timers.gun_hit_due) {
        send_message_to_staff(server, "Player %s (#%hhu) probably has rapid shooting hack", player->name, player->id);
        return 0;
    }
    if (!(diff_is_older_then_dont_update(time_now, player->timers.since_last_block_dest, SPADE_DELAY) &&
          diff_is_older_then_dont_update(time_now, player->timers.since_last_3block_dest, SPADE_DELAY) &&
          diff_is_older_then_dont_update(time_now, player->timers.since_last_block_plac, SPADE_DELAY)))
    {
        send_message_to_staff(server, "Player %s (#%hhu) tried to switch tools and destroy blocks faster then allowed", player->name, player->id);
        return 0;
    }
    uint64_t from = time_now > GUN_HIT_SLACK ? time_now - GUN_HIT_SLACK : 0;
    if (player->timers.gun_hit_due > from) {
        from = player->timers.gun_hit_due;
    }
    player->timers.gun_hit_due                    = from + delay;
    player->timers.since_last_block_dest_with_gun = time_now;
    return 1;
}
