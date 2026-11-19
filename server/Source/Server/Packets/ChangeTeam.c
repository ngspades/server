#include <Server/EventLog.h>
#include <Server/GameApi.h>
#include <Server/Plugins.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Packets/Packets.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Mount.h>
#include <Server/Server.h>
#include <Util/Log.h>

/* A team change: out of the old team (the intel is dropped while player->team
   is still the old one), then into the new one, and a death or a spawn. The
   mode may refuse a crossing (the zombie mode: a player stays where it is). */
void game_player_change_team(server_t* server, player_t* player, uint8_t team)
{
    // @Todo: if player is not initialised yet (pyspades uses if not self.name) ignore this
    uint8_t old_team = player->team;

    if (old_team == team)
        return;

    if (team != TEAM_A && team != TEAM_B && team != TEAM_SPECTATOR) {
        LOG_WARNING("Player %s (#%hhu) sent invalid team. Switching them to Spectator", player->name, player->id);
        team = TEAM_SPECTATOR;
    }

    /* the mode may refuse a crossing (zombie: a player stays in its team) */
    uint8_t chosen = team;
    if (gamemode_team_change(server, player, team, &chosen)) {
        if (chosen != TEAM_A && chosen != TEAM_B && chosen != TEAM_SPECTATOR) {
            chosen = old_team;
        }
        if (chosen == old_team) {
            evt_log(player, "team", "refused why=mode requested=%u", (unsigned) team);
            return;
        }
        team = chosen;
    }

    if (!plugins_on_team(server, player, team)) {
        return;
    }

    /* out of the game for the old team first: dropping the intel works out
       the enemy team from player->team, which must still be the old one */
    if (old_team != TEAM_SPECTATOR) {
        gamemode_player_out(server, player);
    }

    if (server->protocol.num_team_users[old_team] > 0)
        server->protocol.num_team_users[old_team]--;

    player->team = team;
    server->protocol.num_team_users[team]++;
    proto10_bridge_light_off(player, "team");
    mount_team_change(server, player);

    if (old_team == TEAM_SPECTATOR) {
        send_respawn(server, player);
    } else {
        send_kill_action_packet(server, player, player, 5, 5, 0);
    }
}

/* ChangeTeam (29): the player's id, then the team */
void receive_change_team(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in change team packet", player->id, received_id);
    }
    game_player_change_team(server, player, stream_read_u8(data));
}
