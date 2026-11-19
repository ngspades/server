#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Plugins.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/World.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Checks/BlockChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>

/* A line of blocks drawn with a block in hand. The whole line is one edit:
   a protected or a refused cell anywhere refuses all of it, and the count is
   taken once. */
void game_player_block_line(server_t* server, player_t* player, vector3i_t start, vector3i_t end)
{
    if (player->sprinting) {
        return;
    }

    uint64_t time_now = get_nanos();
    if (player->blocks > 0 && player->can_build && server->global_ab && player->item == TOOL_BLOCK &&
        diff_is_older_then(time_now, &player->timers.since_last_block_plac, BLOCK_DELAY) &&
        diff_is_older_then_dont_update(time_now, player->timers.since_last_block_dest, BLOCK_DELAY) &&
        diff_is_older_then_dont_update(time_now, player->timers.since_last_3block_dest, BLOCK_DELAY))
    {
        if (!is_block_placable(server, start) || !is_block_placable(server, end)) {
            return;
        }

        vector3f_t startF = {start.x, start.y, start.z};
        vector3f_t endF   = {end.x, end.y, end.z};
        if (distance_in_3d(endF, player->movement.position) <= 4 && distance_in_3d(startF, player->locAtClick) <= 4 &&
            valid_pos_v3f(server, startF) && valid_pos_v3f(server, endF))
        {
            int size = line_get_blocks(&start, &end, server->s_map.result_line);
            /* the line is one edit: a protected cell anywhere refuses it all,
               like block_action_build does for one block */
            for (int i = 0; i < size; i++) {
                if (!gamemode_cell_editable(server,
                                            server->s_map.result_line[i].x,
                                            server->s_map.result_line[i].y,
                                            server->s_map.result_line[i].z))
                {
                    evt_log(player,
                            "block",
                            "action=build_line cell=%d,%d,%d accepted=0 why=protected",
                            server->s_map.result_line[i].x,
                            server->s_map.result_line[i].y,
                            server->s_map.result_line[i].z);
                    return;
                }
                if (!plugins_on_build(server,
                                      player,
                                      server->s_map.result_line[i].x,
                                      server->s_map.result_line[i].y,
                                      server->s_map.result_line[i].z))
                {
                    return;
                }
            }
            player->blocks -= size;
            proto10_bridge_wallet(server, player);
            for (int i = 0; i < size; i++) {
                map_set_color(&server->s_map.map,
                                 server->s_map.result_line[i].x,
                                 server->s_map.result_line[i].y,
                                 server->s_map.result_line[i].z,
                                 player->tool_color.raw);
                proto10_world_cell(server,
                                   server->s_map.result_line[i].x,
                                   server->s_map.result_line[i].y,
                                   server->s_map.result_line[i].z,
                                   P10_WORLD_CAUSE_PLAYER);
            }
            gamemode_terrain_changed(server, 1);
        }
    }
}

/* BlockLine (14): the player's id, then both ends of the line */
void receive_block_line(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in blockline packet", player->id, received_id);
    }
    vector3i_t start;
    vector3i_t end;
    start.x = stream_read_u32(data);
    start.y = stream_read_u32(data);
    start.z = stream_read_u32(data);
    end.x   = stream_read_u32(data);
    end.y   = stream_read_u32(data);
    end.z   = stream_read_u32(data);
    game_player_block_line(server, player, start, end);
}
