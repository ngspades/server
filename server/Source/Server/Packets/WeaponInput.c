#include <Server/Classes.h>
#include <Server/GameApi.h>
#include <Server/Proto10/Mount.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Packets/Packets.h>
#include <Server/Server.h>
#include <Server/Staff.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <Util/Weapon.h>
#include <stdio.h>

/* The trigger: bit 0 fires, bit 1 aims. A press spends the gun's round, so
   the gun's rate is judged here, not by the hit (the hit only says what the
   round met). */
void game_player_fire_input(server_t* server, player_t* player, uint8_t buttons)
{
    uint8_t mask = 1;
    if (player->state != STATE_READY) {
        return;
    }
    if (player->sprinting) {
        buttons = 0; /* Do not return just set it to 0 as we want to send to players that the player is no longer
                    shooting when they start sprinting */
    }

    player->primary_fire   = buttons & mask;
    player->secondary_fire = (buttons >> 1) & mask;
    /* a seat's trigger is the seat's: the MG's tick reads primary_fire, but
       the gun in hand neither spends a round nor cancels a reload */
    if (mount_mounted(player)) {
        return;
    }
    /* the 0.75 profile tells the others (their clients draw the shots and
       the swings from it); the 1.0 bridge sends the trigger in its updates */
    if (profile_is_075()) {
        legacy_send_weapon_input(server, player, buttons);
    }

    // If the player by some magic decides to shoot
    if (player->reloading) {
        player->reloading = 0;
        if (player->weapon != WEAPON_SHOTGUN) {
            player->next_shot_invalid = 1;
            player->primary_fire      = 0;
            if (profile_is_075()) {
                legacy_send_weapon_reload(server, player, 0);
            }
            return;
        }
    }

    if (player->secondary_fire && player->item == TOOL_BLOCK) {
        player->locAtClick = player->movement.position;
    } else if (player->secondary_fire && player->item == TOOL_SPADE) {
        player->timers.since_possible_spade_nade = get_nanos();
    }

    /* a rocket is spent by the rocket command (Proto10/Rockets.c), not by the
       trigger: the client sends both on one click, and when the trigger came
       first it emptied the clip and the rocket was refused (why=empty) */
    else if (player->weapon_clip > 0 && player->item == TOOL_GUN && player->weapon != GAME_WEAPON_ROCKET)
    {
        uint64_t timeDiff = get_player_weapon_delay_nano(player);

        if (player->primary_fire && diff_is_older_then(get_nanos(), &player->timers.since_last_weapon_input, timeDiff))
        {
            player->timers.since_last_weapon_input = get_nanos();
            player->weapon_clip--;
            if (player->weapon_clip == 0) {
                player->primary_fire   = 0;
                player->secondary_fire = 0;
                if (profile_is_075()) {
                    legacy_send_weapon_input(server, player, 0);
                }
            }
            player->reloading = 0;
            if ((player->movement.previous_orientation.x == player->movement.forward_orientation.x) &&
                (player->movement.previous_orientation.y == player->movement.forward_orientation.y) &&
                (player->movement.previous_orientation.z == player->movement.forward_orientation.z) &&
                player->item == TOOL_GUN)
            {
                player_t *connected_player, *tmp;
                HASH_ITER(hh, server->players, connected_player, tmp)
                {
                    if (is_past_join_screen(connected_player) && is_staff(server, connected_player)) {
                        char message[200];
                        snprintf(message, 200, "WARNING. Player %d may be using no recoil", player->id);
                        send_server_notice(connected_player, 0, message);
                    }
                }
            }
            player->movement.previous_orientation = player->movement.forward_orientation;
        }
    }
}

/* WeaponInput (4): the player's id, then the two bits */
void receive_weapon_input(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id    = stream_read_u8(data);
    uint8_t received_input = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in weapon input packet", player->id, received_id);
    }
    game_player_fire_input(server, player, received_input);
}
