#include <Server/Plugins.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Assists.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Packets/Packets.h>
#include <Server/Conduct.h>
#include <Server/EventLog.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/Nanos.h>
#include <Util/Weapon.h>
#include <time.h>

void send_kill_action_packet(server_t* server,
                             player_t* killer,
                             player_t* player,
                             uint8_t   killReason,
                             uint8_t   respawnTime,
                             uint8_t   makeInvisible)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    if (player->alive == 0) {
        return; // Cant kill player if they are dead
    }

    evt_death(player, killer, killReason);
    proto10_bridge_kill(server, killer, player, killReason, respawnTime);
    /* before the mode's hooks, as the original: the KillAction, then what
       follows from it (the dropped intel) */
    if (profile_is_075()) {
        legacy_send_kill_action(server, killer, player, killReason, respawnTime, makeInvisible);
    }
    /* a team change or an invisible admin's death is no kill to help with */
    if (!makeInvisible && player->is_invisible == 0) {
        if (gamemode_has(server, GM_FEAT_ASSISTS)) {
            assists_on_kill(server, killer, player);
        }
    }

    /* before the mode's hook: the zombie mode moves the victim to the
       killer's team there, and every infection read as a team kill (the
       listen server's host was kicked for "grief" after a few kills) */
    conduct_on_kill(server, killer, player);
    gamemode_player_killed(server, killer, player);
    plugins_on_kill(server, killer, player, killReason);
    /* the carried intel falls where the player died */
    gamemode_player_out(server, player);

    if (!makeInvisible && player->is_invisible == 0) {
        if (killer != player) {
            killer->kills++;
        }
        player->deaths++;
        player->alive                        = 0;
        player->respawn_time                 = respawnTime;
        player->timers.start_of_respawn_wait = get_game_seconds();
        player->state                        = STATE_WAITING_FOR_RESPAWN;
        switch (player->weapon) {
            case WEAPON_RIFLE:
                player->weapon_reserve  = 50;
                player->weapon_clip     = 10;
                player->default_clip    = RIFLE_DEFAULT_CLIP;
                player->default_reserve = RIFLE_DEFAULT_RESERVE;
                if (weapon_is_sniper(player)) {
                    player->weapon_reserve  = GAME_SNIPER_RESERVE;
                    player->weapon_clip     = GAME_SNIPER_CLIP;
                    player->default_clip    = GAME_SNIPER_CLIP;
                    player->default_reserve = GAME_SNIPER_RESERVE;
                }
                break;
            case WEAPON_SMG:
                player->weapon_reserve  = 120;
                player->weapon_clip     = 30;
                player->default_clip    = SMG_DEFAULT_CLIP;
                player->default_reserve = SMG_DEFAULT_RESERVE;
                break;
            case WEAPON_SHOTGUN:
                player->weapon_reserve  = 48;
                player->weapon_clip     = 6;
                player->default_clip    = SHOTGUN_DEFAULT_CLIP;
                player->default_reserve = SHOTGUN_DEFAULT_RESERVE;
                break;
        }
    }
}
