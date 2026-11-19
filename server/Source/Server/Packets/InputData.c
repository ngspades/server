#include <Server/GameApi.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Packets/Packets.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Log.h>

/* The movement keys, once the body is in the game. */
void game_player_move_input(server_t* server, player_t* player, uint8_t keys)
{
    if (player->state != STATE_READY) {
        return;
    }
    uint8_t bits[8];
    uint8_t mask   = 1;
    player->input  = keys;
    for (int i = 0; i < 8; i++) {
        bits[i] = (player->input >> i) & mask;
    }
    player->move_forward   = bits[0];
    player->move_backwards = bits[1];
    player->move_left      = bits[2];
    player->move_right     = bits[3];
    if (!player->airborne) {
        player->jumping = bits[4];
    } else {
        player->jumping = 0;
    }
    player->crouching = bits[5];
    player->sneaking  = bits[6];
    player->sprinting = bits[7];
    if (player->sprinting && player->secondary_fire) {
        // Some game clients dont care to send that they exited scope mode upon sprint start
        player->secondary_fire = 0;
        if (profile_is_075()) {
            legacy_send_weapon_input(server, player, 0);
        }
    }
    if (profile_is_075()) {
        legacy_send_input_data(server, player);
    }
}

/* InputData (3): the player's id, then the key bits */
void receive_input_data(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in Input packet", player->id, received_id);
        return;
    }
    game_player_move_input(server, player, stream_read_u8(data));
}
