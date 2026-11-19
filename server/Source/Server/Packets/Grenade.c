#include <Server/Anticheat.h>
#include <Server/EventLog.h>
#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Plugins.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Server.h>
#include <Server/Staff.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Checks/VectorChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>
#include <Server/Proto10/Entities.h>
#include <Server/Proto10/Mount.h>
#include <math.h>

/* A grenade leaves the hand. The client says where it starts and how fast it
   goes (blocks per 1/32 s on the 0.75 wire, the same numbers a 1.0
   ThrowGrenade carries); the server caps the fuse, refuses a body that runs
   or a hand that is not holding the grenade, and keeps the throw's speed
   within what a player can throw. */
void game_player_throw_grenade(server_t* server,
                               player_t* player,
                               float      fuse,
                               vector3f_t position,
                               vector3f_t velocity)
{
    if (gamemode_player_rules(server, player) & GM_RULE_NO_GRENADE) {
        return;
    }
    if (mount_mounted(player)) {
        /* a seat is no throwing position (Mount.c, 12.8) */
        evt_log(player, "grenade", "action=refused why=mounted");
        return;
    }
    if (player->item != TOOL_GRENADE) {
        send_server_notice(player, 0, "InstaSuicideNade detected. Grenade ineffective");
        send_message_to_staff(server, "Player %s (#%hhu) tried to use InstaSpadeNade", player->name, player->id);
        return;
    }

    if (player->sprinting) {
        return;
    }

    uint64_t timeNow = get_nanos();
    if (!diff_is_older_then(timeNow, &player->timers.since_last_grenade_thrown, NANO_IN_MILLI * 500) ||
        !diff_is_older_then_dont_update(timeNow, player->timers.since_possible_spade_nade, (long) NANO_IN_MILLI * 1000))
    {
        return;
    }

    if (player->grenades > 0 && !plugins_on_grenade(server, player)) {
        return;
    }
    if (player->grenades > 0) {
        grenade_t* grenade = spadesx_malloc(sizeof(grenade_t));

        if (isnan(fuse) || isinf(fuse)) {
            free(grenade);
            return;
        }

        grenade->fuse       = fminf(3.0f, fuse);
        grenade->position.x = position.x;
        grenade->position.y = position.y;
        grenade->position.z = position.z;

        if (!valid_vec3f(grenade->position)) {
            anticheat_report(server, player, ANTICHEAT_GRENADE, 0, "position");
            free(grenade);
            return;
        }

        grenade->velocity.x = velocity.x;
        grenade->velocity.y = velocity.y;
        grenade->velocity.z = velocity.z;

        if (!valid_vec3f(grenade->velocity)) {
            anticheat_report(server, player, ANTICHEAT_GRENADE, 0, "velocity");
            free(grenade);
            return;
        }

        vector3f_t relative = {grenade->velocity.x - player->movement.velocity.x,
                               grenade->velocity.y - player->movement.velocity.y,
                               grenade->velocity.z - player->movement.velocity.z};
        float      length = sqrt((relative.x * relative.x) + (relative.y * relative.y) + (relative.z * relative.z));
        if (length > 2) {
            float lenghtNorm  = 1 / length;
            relative.x        = ((relative.x * lenghtNorm) * 2) + player->movement.velocity.x;
            relative.y        = ((relative.y * lenghtNorm) * 2) + player->movement.velocity.y;
            relative.z        = ((relative.z * lenghtNorm) * 2) + player->movement.velocity.z;
            grenade->velocity = relative;
        }
        if (valid_pos_v3f(server, grenade->position) ||
            (valid_pos_v3f_below_z(server, grenade->position) && player->movement.position.z < 0))
        {
            grenade->sent            = 1;
            grenade->time_since_sent = get_nanos();
        }
        DL_APPEND(player->grenade, grenade);
        /* a 0.75 grenade is a packet to the others, not an entity; only an
           armed one, as the original (where send_grenade sat) */
        if (profile_is_075() && grenade->sent) {
            legacy_send_grenade(server, player, grenade->fuse, grenade->position, grenade->velocity);
        }
        proto10_entities_grenade_thrown(server, player, grenade);
        player->grenades--;
    }
}

/* Grenade (6): the player's id, then the fuse, where it starts and its
   velocity. The fields are only sent when the throw is possible; a body
   that cannot throw has them read and ignored. */
void receive_grenade_packet(server_t* server, player_t* player, stream_t* data)
{
    uint8_t ID = stream_read_u8(data);
    if (player->id != ID) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in grenade packet", player->id, ID);
    }
    float      fuse = stream_read_f(data);
    float      px = stream_read_f(data), py = stream_read_f(data), pz = stream_read_f(data);
    float      vx = stream_read_f(data), vy = stream_read_f(data), vz = stream_read_f(data);
    game_player_throw_grenade(
        server, player, fuse, (vector3f_t) {px, py, pz}, (vector3f_t) {vx, vy, vz});
}
