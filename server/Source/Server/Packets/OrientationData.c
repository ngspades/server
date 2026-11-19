#include <Server/Anticheat.h>
#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Server.h>
#include <Util/Checks/VectorChecks.h>
#include <Util/Physics.h>
#include <math.h>

/* Where a body looks: the forward vector, normalised when longer than 1. */
void game_player_look(server_t* server, player_t* player, vector3f_t forward_orientation)
{
    if ((forward_orientation.x + forward_orientation.y + forward_orientation.z) == 0) {
        return;
    }

    if (!valid_vec3f(forward_orientation)) {
        anticheat_report(server, player, ANTICHEAT_ORIENTATION, 0, "non_finite");
        return;
    }

    float length = sqrt((forward_orientation.x * forward_orientation.x) +
                        (forward_orientation.y * forward_orientation.y) +
                        (forward_orientation.z * forward_orientation.z));
    float norm_length = 1 / length;

    // Normalize the vectors if their length > 1
    if (length > 1.f) {
        player->movement.forward_orientation.x = forward_orientation.x * norm_length;
        player->movement.forward_orientation.y = forward_orientation.y * norm_length;
        player->movement.forward_orientation.z = forward_orientation.z * norm_length;
    } else {
        player->movement.forward_orientation.x = forward_orientation.x;
        player->movement.forward_orientation.y = forward_orientation.y;
        player->movement.forward_orientation.z = forward_orientation.z;
    }

    physics_reorient_player(player, &player->movement.forward_orientation);
}

/* OrientationData (1): three floats */
void receive_orientation_data(server_t* server, player_t* player, stream_t* data)
{
    float x = stream_read_f(data);
    float y = stream_read_f(data);
    float z = stream_read_f(data);
    game_player_look(server, player, (vector3f_t) {x, y, z});
}
