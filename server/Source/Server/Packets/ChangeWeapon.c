#include <Server/EventLog.h>
#include <Server/GameApi.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Plugins.h>
#include <Server/Packets/Packets.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/Log.h>
#include <Util/Weapon.h>

/* A new gun, from the weapon screen. It costs a death, except in the arena's
   countdown, where it is free (the player is behind a closed gate). */
void game_player_change_weapon(server_t* server, player_t* player, uint8_t weapon)
{
    if (player->weapon == weapon) {
        return;
    }
    if (!plugins_on_weapon(server, player, weapon)) {
        return;
    }
    if (gamemode_weapon_change_instant(server, player)) {
        /* the arena's countdown: the player stands behind the closed gate
           where nothing can happen to it, so the new gun is taken where it
           is, full and without a death (a 0.75 change elsewhere kills) */
        player->weapon = weapon;
        set_default_player_ammo(player);
        set_default_player_pellets(player);
        player->reloading = 0;
        evt_log(player, "arena", "action=weapon_swap weapon=%d instant=1", weapon);
        /* the weapon and its skin only travel in a Create: give every 1.0
           client the body again, as a respawn tells them */
        proto10_bridge_body_recreate(player);
        return;
    }
    player->weapon = weapon;
    send_kill_action_packet(server, player, player, 6, 5, 0);
    player->state = STATE_WAITING_FOR_RESPAWN;
}

/* ChangeWeapon (30): the player's id, then the gun */
void receive_change_weapon(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in change weapon packet", player->id, received_id);
    }
    game_player_change_weapon(server, player, stream_read_u8(data));
}
