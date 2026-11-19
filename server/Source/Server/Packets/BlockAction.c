#include <Server/Block.h>
#include <Server/Bots.h>
#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Nodes.h>
#include <Server/Anticheat.h>
#include <Server/Server.h>
#include <Server/Staff.h>
#include <Server/Structs/CommandStruct.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>
#include <stdint.h>

/* One block built or dug, at a cell. The tool must match the action (a spade
   digs, blocks build, a gun may break one) and the body must be allowed to
   build at all. The map change and the bots that hear it follow. */
void game_player_block_action(server_t* server, player_t* player, uint8_t action_type, uint32_t X, uint32_t Y, uint32_t Z)
{
    if (!(player->can_build && server->global_ab) || player->sprinting) {
        return;
    }
    vector3i_t vector_block  = {X, Y, Z};
    vector3f_t vectorf_block = {(float) X, (float) Y, (float) Z};
    vector3f_t player_vector = player->movement.position;
    if (!((player->item == TOOL_SPADE && (action_type == 1 || action_type == 2)) ||
          (player->item == TOOL_BLOCK && action_type == 0) || (player->item == TOOL_GUN && action_type == 1)))
    {
        anticheat_report(server, player, ANTICHEAT_BLOCK, 1, "item_action");
        LOG_WARNING("Player %s (#%hhu) may be using BlockExploit with Item: %d and Action: %d",
                    player->name,
                    player->id,
                    player->item,
                    action_type);
        return;
    }
    uint8_t was_solid = valid_pos_v3i(server, vector_block) && map_solid(&server->s_map.map, X, Y, Z);
    uint8_t was_built = action_type == 0 && !was_solid;
    handle_block_action(server, player, action_type, vector_block, vectorf_block, player_vector, X, Y, Z);
    gamemode_terrain_changed(server, 0);
    /* a spade that opened a cell makes a noise the enemy bots hear */
    if (action_type != 0 && player->item == TOOL_SPADE && was_solid &&
        !map_solid(&server->s_map.map, X, Y, Z))
    {
        bots_on_dig(server, player, (int) X, (int) Y, (int) Z);
    }
    if (was_built && map_solid(&server->s_map.map, X, Y, Z)) {
        bots_on_build(server, player, (int) X, (int) Y, (int) Z);
    }
}

/* BlockAction (13): the player's id, then the action and the cell */
void receive_block_action(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in block action packet", player->id, received_id);
    }
    uint8_t  action = stream_read_u8(data);
    uint32_t X      = stream_read_u32(data);
    uint32_t Y      = stream_read_u32(data);
    uint32_t Z      = stream_read_u32(data);
    game_player_block_action(server, player, action, X, Y, Z);
}
