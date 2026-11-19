#include <Server/Player.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Events.h>
#include <Server/ParseConvert.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/Log.h>

void send_player_left(server_t* server, player_t* player)
{
    char ipString[64];
    player_address(player, ipString, sizeof ipString);
    LOG_INFO("Player %s (%s, #%hhu) disconnected", player->name, ipString, player->id);

    on_player_disconnect_run(server, player);
    /* the 1.0 PlayerRemove: without it a removed bot stayed on every client
       as a ghost that could be shot forever ("hit player who doesn't exist") */
    proto10_bridge_player_left(server, player);
    if (profile_is_075()) {
        legacy_send_player_left(server, player);
    }

    if (server->protocol.num_players == 0) {
        return;
    }
}
