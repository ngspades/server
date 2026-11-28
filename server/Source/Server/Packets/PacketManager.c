#include <Server/LagComp.h>
#include <Server/Packets/Packets.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>

inline uint8_t allow_shot(server_t*  server,
                          player_t*  player,
                          player_t*  player_hit,
                          uint64_t   time_now,
                          float      distance,
                          long*      x,
                          long*      y,
                          long*      z,
                          vector3f_t shot_pos,
                          vector3f_t shot_orien,
                          vector3f_t hit_pos,
                          vector3f_t shot_eye_pos,
                          uint8_t    last_round)
{
    uint8_t ret = 0;

    if (player->next_shot_invalid) {
        player->next_shot_invalid = 0;
        return ret;
    }

    /* the round that emptied the clip let go of the trigger (WeaponInput.c,
       Player.c) before its hit came in: that hit still counts */
    if ((player->primary_fire || last_round) && player->reloading == 0 &&
        ((player->item == TOOL_SPADE && diff_is_older_then(time_now, &player->timers.since_last_shot, NANO_IN_MILLI * 100)) ||
         (player->item == TOOL_GUN && player->weapon_pellets != 0 && player_gun_raised(player, time_now))) &&
        player->alive && player_hit->alive && (player->team != player_hit->team || player->allow_team_killing) &&
        (player->allow_killing && server->global_ak) && physics_validate_hit(shot_pos, shot_orien, hit_pos, lagcomp_hit_tolerance()) &&
        (physics_cast_ray(server,
                          shot_eye_pos.x,
                          shot_eye_pos.y,
                          shot_eye_pos.z,
                          shot_orien.x,
                          shot_orien.y,
                          shot_orien.z,
                          distance,
                          x,
                          y,
                          z) == 0 ||
         physics_cast_ray(server,
                          shot_eye_pos.x,
                          shot_eye_pos.y,
                          shot_eye_pos.z - 1,
                          shot_orien.x,
                          shot_orien.y,
                          shot_orien.z,
                          distance,
                          x,
                          y,
                          z) == 0))
    {
        ret = 1;
    }

    return ret;
}
