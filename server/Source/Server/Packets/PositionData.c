#include <Server/Anticheat.h>
#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/VectorChecks.h>
#include <Util/Log.h>

/* A body's position. Refused when it is not finite, when it is too far from
   where the server has it (a 1.0 client is sent a correction, a 0.75 one its
   own position back) and when it is inside the ground. */
void game_player_move(server_t* server, player_t* player, vector3f_t position)
{
    if (!valid_vec3f(position)) {
        anticheat_report(server, player, ANTICHEAT_POSITION, 0, "non_finite");
        return;
    }

    /* too far from where the server has it: ignored, and the 1.0 bridge
       answers with a correction (Proto10/Bridge.c _apply_own_entity) */
    if (distance_in_3d(player->movement.position, position) >= 6) {
        /* a 0.75 client is told where the server has it (a /tp too) */
        if (profile_is_075()) {
            legacy_send_position(
                player, player->movement.position.x, player->movement.position.y, player->movement.position.z);
        }
        return;
    }

    if (player->movement.position.z > 0) {
        uint16_t x = player->movement.position.x;
        uint16_t y = player->movement.position.y;
        uint16_t z = player->movement.position.z;

        if (z <= server->s_map.map.size_z - 2 && map_solid(&server->s_map.map, x, y, z) &&
            map_solid(&server->s_map.map, x, y, z + 1) && map_solid(&server->s_map.map, x, y, z + 2))
        {
            return;
        }
    }

    player->movement.prev_position = player->movement.position;
    player->movement.position.x    = position.x;
    player->movement.position.y    = position.y;
    player->movement.position.z    = position.z;

    if (valid_player_pos(server, player, position.x, position.y, position.z)) {
        player->movement.prev_legit_pos = player->movement.position;
    }
}

/* PositionData (0): three floats */
void receive_position_data(server_t* server, player_t* player, stream_t* data)
{
    float x = stream_read_f(data);
    float y = stream_read_f(data);
    float z = stream_read_f(data);
    game_player_move(server, player, (vector3f_t) {x, y, z});
}
