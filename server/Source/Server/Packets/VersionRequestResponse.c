#include <Server/Server.h>
#include <Util/Notice.h>
#include <stdio.h>

void receive_version_response(server_t* server, player_t* player, stream_t* data)
{
    (void) server;
    player->client           = stream_read_u8(data);
    player->version_major    = stream_read_u8(data);
    player->version_minor    = stream_read_u8(data);
    player->version_revision = stream_read_u8(data);
    uint32_t length          = stream_left(data);
    if (length < 255) {
        player->os_info[length] = '\0';
        stream_read_array(data, player->os_info, length);
    } else {
        snprintf(player->os_info, 8, "Unknown");
    }
    if (player->client == 'o') {
        if (!(player->version_major == 0 && player->version_minor == 1 &&
              (player->version_revision == 3 || player->version_revision == 5)))
        {
            notice_kick(player, REASON_KICKED, CONN_CLOSE, "This version of OpenSpades is not supported here");
        }
    } else if (player->client == 'B') {
        if (!(player->version_major == 0 && player->version_minor == 1 && player->version_revision == 5)) {
            notice_kick(player, REASON_KICKED, CONN_CLOSE, "This version of BetterSpades is not supported here");
        }
    }
}
