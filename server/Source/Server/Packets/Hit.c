#include <Server/GameApi.h>
#include <Server/Plugins.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Anticheat.h>
#include <Server/LagComp.h>
#include <Server/SpawnProtect.h>
#include <Server/Packets/Packets.h>
#include <Server/Proto10/Mount.h>
#include <Server/Server.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Weapon.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <classes/classes.h>
#include <uthash/uthash.h>
#include <string.h>

/* allow_shot() is one condition; this names the part that failed, so a hit
   that does nothing says why in the event log */
static const char* _hit_refusal(server_t*  server,
                                player_t*  player,
                                player_t*  hit_player,
                                vector3f_t shot_pos,
                                vector3f_t shot_orien,
                                vector3f_t aim_pos)
{
    if (player->next_shot_invalid) {
        return "shot_invalidated";
    }
    if (!player->primary_fire) {
        return "not_firing";
    }
    if (player->reloading) {
        return "reloading";
    }
    if (player->item == TOOL_GUN && player->weapon_pellets == 0) {
        return "no_pellets";
    }
    if (player->item != TOOL_GUN && player->item != TOOL_SPADE) {
        return "wrong_tool";
    }
    if (!player->alive || !hit_player->alive) {
        return "dead";
    }
    if (player->team == hit_player->team && !player->allow_team_killing) {
        return "same_team";
    }
    if (!player->allow_killing || !server->global_ak) {
        return "killing_disabled";
    }
    if (!physics_validate_hit(shot_pos, shot_orien, aim_pos, lagcomp_hit_tolerance())) {
        return "not_aimed_at_target";
    }
    /* last: allow_shot already moved the swing timer, so only a swing that
       passed every other check reads as too fast (it was first, and hid
       every other reason a spade or claw hit was refused) */
    if (player->item == TOOL_SPADE) {
        return "melee_too_fast_or_wall";
    }
    return "terrain_in_the_way";
}

/* A shot or a swing that met a body: the victim's id and the part hit. Every
   rule a shot has (the trigger, the gun's rate, the team, the sight line, the
   spawn protection, the plugins) is judged here, for both wires and the bots.
   `hit_type` is HIT_TYPE_* (Util/Enums.h). */
void game_player_hit(server_t* server, player_t* player, uint8_t hit_player_id, uint8_t hit_type)
{
    player_t* hit_player;
    HASH_FIND(hh, server->players, &hit_player_id, sizeof(hit_player_id), hit_player);
    if (hit_player == NULL) {
        LOG_WARNING("Player %s (#%hhu) hit player who doesn't exist (#%hhu)", player->name, player->id, hit_player_id);
        return;
    }
    /* a seat is not a firing position, and a helicopter's hull is between its
       occupants and any bullet (Proto10/Mount.c, 12.8) */
    if (mount_mounted(player)) {
        evt_log(player, "hit_refused", "target=%s why=mounted", hit_player->name);
        return;
    }
    if (mount_in_vehicle(hit_player)) {
        evt_log(player, "hit_refused", "target=%s why=in_vehicle", hit_player->name);
        return;
    }
    vector3f_t shot_pos     = player->movement.position;
    vector3f_t shot_eye_pos = player->movement.eye_pos;
    vector3f_t hit_pos      = hit_player->movement.position;
    vector3f_t aim_pos      = hit_pos;
    lagcomp_rewind(server, player, hit_player, &aim_pos);
    vector3f_t shot_orien   = player->movement.forward_orientation;
    float      distance     = distance_in_2d(shot_pos, hit_pos);
    long       x = 0, y = 0, z = 0;

    /* the trigger's press spends the round before the hit is read: the shot
       that emptied the clip is still in its gun's interval, and its hit
       counts. Refused, the last round of every clip did no damage */
    int empty = player->item == TOOL_GUN && player->weapon_clip == 0 &&
                diff_is_older_then_dont_update(
                    get_nanos(), player->timers.since_last_weapon_input, get_player_weapon_delay_nano(player));
    if (player->sprinting || empty) {
        // Sprinting and hitting somebody is impossible
        evt_log(player, "hit_refused", "target=%s why=%s", hit_player->name,
                player->sprinting ? "sprinting" : "empty_clip");
        return;
    }

    if (hit_type != HIT_TYPE_MELEE && (gamemode_player_rules(server, player) & GM_RULE_NO_GUN)) {
        evt_log(player, "hit_refused", "target=%s why=mode", hit_player->name);
        return;
    }
    if (!plugins_on_hit(server, player, hit_player, hit_type)) {
        return;
    }
    uint64_t timeNow = get_nanos();

    if (!allow_shot(
        server, player, hit_player, timeNow, distance, &x, &y, &z, shot_pos, shot_orien, aim_pos, shot_eye_pos))
    {
        const char* why = _hit_refusal(server, player, hit_player, shot_pos, shot_orien, aim_pos);
        evt_log(player, "hit_refused", "target=%s why=%s", hit_player->name, why);
        if (!strcmp(why, "not_aimed_at_target") || !strcmp(why, "shot_invalidated")) {
            anticheat_report(server, player, ANTICHEAT_HIT, 1, why);
        }
        return;
    }
    spawnprotect_clear(player);
    if (player->team != hit_player->team && spawnprotect_active(hit_player)) {
        evt_log(player, "hit_refused", "target=%s why=spawn_protect", hit_player->name);
        proto10_bridge_hit_protected(player, hit_player);
        return;
    }
    player->shots++;
    {
        if(player->item == TOOL_GUN && player->weapon_pellets != 0) {
            player->weapon_pellets--;
        }

        switch ((int) player->weapon) {
            case WEAPON_RIFLE:
            {
                switch (hit_type) {
                    case HIT_TYPE_HEAD:
                    {
                        send_kill_action_packet(server, player, hit_player, 1, 5, 0);
                        break;
                    }
                    case HIT_TYPE_TORSO:
                    {
                        /* a classed player's rifle is the Marksman's sniper: 50 like
                           BattleSpades' (two hits kill; the classic 49 left 2 hp) */
                        send_set_hp(server, player, hit_player, classes_of(server, player) != NULL ? GAME_SNIPER_BODY : 49, 1,
                                    0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_ARMS:
                    case HIT_TYPE_LEGS:
                    {
                        send_set_hp(server, player, hit_player, classes_of(server, player) != NULL ? GAME_SNIPER_BODY : 33, 1,
                                    0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_MELEE:
                        // Empty so we dont have errors :)
                        break;
                }
                break;
            }
            case WEAPON_SMG:
            {
                switch (hit_type) {
                    case HIT_TYPE_HEAD:
                    {
                        send_set_hp(server, player, hit_player, 75, 1, 1, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_TORSO:
                    {
                        send_set_hp(server, player, hit_player, 29, 1, 0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_ARMS:
                    {
                        send_set_hp(server, player, hit_player, 18, 1, 0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_LEGS:
                    {
                        send_set_hp(server, player, hit_player, 18, 1, 0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_MELEE:
                        // Empty so we dont have errors :)
                        break;
                }
                break;
            }
            case WEAPON_SHOTGUN:
            {
                switch (hit_type) {
                    case HIT_TYPE_HEAD:
                    {
                        send_set_hp(server, player, hit_player, 37, 1, 1, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_TORSO:
                    {
                        send_set_hp(server, player, hit_player, 27, 1, 0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_ARMS:
                    {
                        send_set_hp(server, player, hit_player, 16, 1, 0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_LEGS:
                    {
                        send_set_hp(server, player, hit_player, 16, 1, 0, 5, 0, shot_pos);
                        break;
                    }
                    case HIT_TYPE_MELEE:
                        // Empty so we dont have errors :)
                        break;
                }
                break;
            }
            /* the revolver, a class's second gun (common/classes/classes.h) */
            case GAME_WEAPON_REVOLVER:
            {
                if (hit_type == HIT_TYPE_HEAD) {
                    send_set_hp(server, player, hit_player, GAME_REVOLVER_HEAD, 1, 1, 5, 0, shot_pos);
                } else if (hit_type == HIT_TYPE_TORSO) {
                    send_set_hp(server, player, hit_player, GAME_REVOLVER_TORSO, 1, 0, 5, 0, shot_pos);
                } else if (hit_type == HIT_TYPE_ARMS || hit_type == HIT_TYPE_LEGS) {
                    send_set_hp(server, player, hit_player, GAME_REVOLVER_LIMB, 1, 0, 5, 0, shot_pos);
                }
                break;
            }
        }
        if (hit_type == HIT_TYPE_MELEE) {
            /* the class's spade (or pickaxe) does its own damage (retail: 35
               or 50); the classic 80 stays for a player without one */
            int melee = classes_melee(server, player);
            send_set_hp(server, player, hit_player, melee > 0 ? melee : 80, 1, 2, 5, 0, shot_pos);
        }
    }
}

/* HitEntity (5): the victim's id, then the part hit */
void receive_hit_packet(server_t* server, player_t* player, stream_t* data)
{
    uint8_t hit_player_id = stream_read_u8(data);
    uint8_t hit_type      = stream_read_u8(data);
    game_player_hit(server, player, hit_player_id, hit_type);
}
