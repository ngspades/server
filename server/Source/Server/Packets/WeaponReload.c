#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Plugins.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Log.h>
#include <Util/Nanos.h>

/* A reload. The server keeps its own clip; the trigger is released and the
   others are told (a 0.75 client hears the sound, a 1.0 one gets the entity
   event), so a body that reloads looks like it. */
void game_player_reload(server_t* server, player_t* player)
{
    player->primary_fire   = 0;
    player->secondary_fire = 0;

    if (player->weapon_reserve == 0 || player->reloading) {
        return;
    }
    player->reloading                 = 1;
    player->timers.since_reload_start = get_nanos();
    /* the others hear it: nobody was told, and a bot never seemed to reload */
    if (profile_is_075()) {
        legacy_send_weapon_reload(server, player, 1);
    } else {
        proto10_bridge_reload(server, player);
    }
    plugins_on_reload(server, player);
}

/* WeaponReload (28): the player's id, then the client's clip and reserve,
   which the server does not trust (it keeps its own counts) */
void receive_weapon_reload(server_t* server, player_t* player, stream_t* data)
{
    uint8_t ID = stream_read_u8(data);
    if (player->id != ID) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in weapon reload packet", player->id, ID);
    }
    game_player_reload(server, player);
}
