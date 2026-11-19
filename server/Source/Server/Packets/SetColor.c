#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Log.h>

/* The colour a player builds in. */
void game_player_set_color(server_t* server, player_t* player, color_t color)
{
    player->tool_color = color;
    /* a 0.75 client builds in the colour it was last told of the builder */
    if (profile_is_075()) {
        legacy_send_set_color(server, player, color);
    }
}

/* SetColor (8): the player's id, then the colour as b, g, r */
void receive_set_color(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    color_t color       = stream_read_color_rgb(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in set color packet", player->id, received_id);
    }
    game_player_set_color(server, player, color);
}
