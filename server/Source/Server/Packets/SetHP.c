#include <Server/Plugins.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Assists.h>
#include <Util/Nanos.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Packets/Packets.h>
#include <Server/Bots.h>
#include <Server/Conduct.h>
#include <Server/SpawnProtect.h>
#include <Server/EventLog.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>

void send_set_hp(server_t*  server,
                 player_t*  player,
                 player_t*  player_hit,
                 long       hp_chnage,
                 uint8_t    type_of_damage,
                 uint8_t    kill_reason,
                 uint8_t    respawn_time,
                 uint8_t    is_grenade,
                 vector3f_t position)
{

    if (server->protocol.num_players == 0 || player_hit->team == TEAM_SPECTATOR ||
        (!player->allow_team_killing && player->team == player_hit->team && player->id != player_hit->id))
    {
        return;
    }
    /* a fresh spawn: enemy fire is refused, environmental damage is not */
    if (player != player_hit && type_of_damage != 0 && player->team != player_hit->team &&
        spawnprotect_active(player_hit))
    {
        evt_log(player, "hit_refused", "target=%s why=spawn_protect", player_hit->name);
        return;
    }
    if ((player->allow_killing && server->global_ak && player->alive) || type_of_damage == 0) {
        hp_chnage = gamemode_hit_damage(server, player, player_hit, hp_chnage, kill_reason);
        if (hp_chnage > 0 && !plugins_on_damage(server, player, player_hit, &hp_chnage, kill_reason)) {
            return;
        }
        if (hp_chnage > player_hit->hp) {
            hp_chnage = player_hit->hp;
        }
        player_hit->hp -= hp_chnage;
        if (hp_chnage > 0) {
            if (player != player_hit) {
                player->hits++;
            }
            conduct_on_damage(server, player, player_hit, hp_chnage);
            player_hit->hurt_at = get_nanos();
            bots_on_damage(server, player, player_hit);
            assists_on_damage(player, player_hit, hp_chnage);
        }
        evt_damage(player, player_hit, hp_chnage, type_of_damage);
        /* the ZeroSpades damage number, a killing blow too; no attacker for
           a fall, the water or the boundary */
        if (profile_is_075() && hp_chnage != 0) {
            ext075_on_damage(server, (type_of_damage == 0 || player == player_hit) ? NULL : player, player_hit,
                             (int) hp_chnage);
        }
        if (player_hit->hp < 0)
            player_hit->hp = 0;

        else if (player_hit->hp > 100)
            player_hit->hp = 100;

        if (player_hit->hp == 0) {
            send_kill_action_packet(server, player, player_hit, kill_reason, respawn_time, 0);
            return;
        }

        else if (player_hit->hp > 0 && player_hit->hp < 100)
        {
            if (profile_is_075()) {
                legacy_send_set_hp(player, player_hit, type_of_damage, is_grenade, position);
                ext075_player_changed(server, player_hit);
                return;
            }
            /* kill reason 1 is the 0.75 headshot (Hit.c) */
            proto10_bridge_damage(
                server, player, player_hit, hp_chnage, type_of_damage, is_grenade, kill_reason, position);
        }
    }
}
