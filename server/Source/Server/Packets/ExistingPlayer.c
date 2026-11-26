#include <Server/Player.h>
#include <Server/GameApi.h>
#include <Server/EventLog.h>
#include <Server/Plugins.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Names.h>
#include "Server/Structs/PlayerStruct.h"
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Alloc.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Util/Weapon.h>
#include <ctype.h>

/* A player entering the game with its team and gun chosen: the name is
   applied (sanitised, unique), the mode may seat it somewhere else (the
   zombie mode), a plugin may refuse it (on_join) and the welcome lines go
   out. The body spawns from here (STATE_SPAWNING). */
void game_player_join(server_t*   server,
                      player_t*   player,
                      uint8_t     team,
                      uint8_t     weapon,
                      uint8_t     tool,
                      uint32_t    kills,
                      const char* raw_name)
{
    if (player->team != TEAM_SPECTATOR) {
        return;
    }
    player->team   = team;
    player->weapon = weapon;
    player->item   = tool;
    player->kills  = kills;

    if (player->team != TEAM_A && player->team != TEAM_B && player->team != TEAM_SPECTATOR) {
        LOG_WARNING("Player %s (#%hhu) sent invalid team. Switching them to Spectator", player->name, player->id);
        player->team = TEAM_SPECTATOR;
    }

    /* the mode decides where a joiner lands (zombie: a spectator joins the
       infected in a fight, a survivor outside it) */
    uint8_t chosen = player->team;
    if (gamemode_join_team(server, player, player->team, &chosen)) {
        if (chosen != TEAM_A && chosen != TEAM_B && chosen != TEAM_SPECTATOR) {
            chosen = TEAM_SPECTATOR;
        }
        if (chosen != player->team) {
            evt_log(player, "team", "join_mode requested=%u chosen=%u", (unsigned) player->team, (unsigned) chosen);
            player->team = chosen;
        }
    }

    /* a full team (the mode's limit, 5 v 5) takes a person in place of a
       bot; with no bot to give, the other team, else the spectators */
    if ((player->team == TEAM_A || player->team == TEAM_B) && !gamemode_team_admit(server, player->team)) {
        uint8_t other = player->team == TEAM_A ? TEAM_B : TEAM_A;
        uint8_t to    = gamemode_team_admit(server, other) ? other : TEAM_SPECTATOR;
        evt_log(player, "team", "refused why=full requested=%u chosen=%u max=%d", (unsigned) player->team,
                (unsigned) to, gamemode_team_max(server));
        send_server_notice(player, 0, "That team is full: %d players at most.", gamemode_team_max(server));
        player->team = to;
    }

    if (player->team != TEAM_SPECTATOR) {
        server->protocol.num_team_users[player->team]++;
    }

    player->ups = 60;

    uint8_t invName = names_apply(server, player, (char*) raw_name, player->name, PLAYER_NAME_STRLEN + 1);

    /* a plugin may send the player to another team, or keep it out (on_join) */
    uint8_t wanted = player->team;
    if (!plugins_on_join(server, player, &wanted)) {
        wanted = TEAM_SPECTATOR;
    }
    if (wanted != player->team) {
        if (player->team != TEAM_SPECTATOR && server->protocol.num_team_users[player->team] > 0) {
            server->protocol.num_team_users[player->team]--;
        }
        player->team = wanted;
        if (wanted != TEAM_SPECTATOR) {
            server->protocol.num_team_users[wanted]++;
        }
    }
    set_default_player_ammo(player);
    player->state = STATE_SPAWNING;
    char IP[64];
    player_address(player, IP, sizeof IP);
    char team_name[15];
    team_id_to_str(server, team_name, player->team);
    LOG_INFO("Player %s (%s, #%hhu) joined %s", player->name, IP, player->id, team_name);
    if (player->welcome_sent == 0) {
        string_node_t* welcomeMessage;
        DL_FOREACH(server->welcome_messages, welcomeMessage)
        {
            send_server_notice(player, 0, welcomeMessage->string);
        }
        if (invName) {
            send_server_notice(player,
                               0,
                               "Your name was either empty, had # in front of it, had unreadable character in it or "
                               "contained something nasty. Your name "
                               "has been set to %s",
                               player->name);
        }
        player->welcome_sent = 1; // So we dont send the message to the player on each time they spawn.
    }
}

/* ExistingPlayer (9): a "dumb" id (the client has none yet), then the team,
   the gun, the tool, the kills, a colour the original had (unused) and the
   name. At most PLAYER_NAME_STRLEN characters of the name are taken. */
void receive_existing_player(server_t* server, player_t* player, stream_t* data)
{
    stream_skip(data, 1); // Clients always send a "dumb" ID here since server has not sent them their ID yet
    uint8_t  team   = stream_read_u8(data);
    uint8_t  weapon = stream_read_u8(data);
    uint8_t  tool   = stream_read_u8(data);
    uint32_t kills  = stream_read_u32(data);
    stream_read_color_rgb(data);

    uint32_t length = stream_left(data);
    char     raw_name[PLAYER_NAME_STRLEN + 1] = {0};
    if (length > PLAYER_NAME_STRLEN) {
        LOG_WARNING("Name of player %u is too long. Cutting", player->id);
        length = PLAYER_NAME_STRLEN;
    }
    stream_read_array(data, raw_name, length);
    raw_name[PLAYER_NAME_STRLEN] = '\0';

    game_player_join(server, player, team, weapon, tool, kills, raw_name);
}
