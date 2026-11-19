#ifndef COMMANDSMANAGER_H
#define COMMANDSMANAGER_H

#include <Server/Server.h>

/* ipv6: the /64 of an IPv6 player (conn_ipv6_prefix), banned instead of ip;
   NULL for an IPv4 ban */
void cmd_generate_ban(server_t*      server,
                      command_args_t arguments,
                      float          time,
                      ip_t           ip,
                      const char*    ipv6,
                      char*          reason);

void cmd_admin(void* p_server, command_args_t arguments);
void cmd_ban_custom(void* p_server, command_args_t arguments);
void cmd_ban_range(void* p_server, command_args_t arguments);
void cmd_admin_mute(void* p_server, command_args_t arguments);
void cmd_clin(void* p_server, command_args_t arguments);
void cmd_help(void* p_server, command_args_t arguments);
void cmd_intel(void* p_server, command_args_t arguments);
void cmd_inv(void* p_server, command_args_t arguments);
void cmd_kick(void* p_server, command_args_t arguments);
void cmd_kill(void* p_server, command_args_t arguments);
void cmd_login(void* p_server, command_args_t arguments);
void cmd_logout(void* p_server, command_args_t arguments);
void cmd_mute(void* p_server, command_args_t arguments);
void cmd_pm(void* p_server, command_args_t arguments);
void cmd_ratio(void* p_server, command_args_t arguments);
void cmd_reset(void* p_server, command_args_t arguments);
void cmd_say(void* p_server, command_args_t arguments);
void cmd_server(void* p_server, command_args_t arguments);
void cmd_profile(void* p_server, command_args_t arguments);

/* the 0.75 profile's player cap: the 0.75 limit, which every 0.75 client
   and master expects (the 1.0 profile has SERVER_MAX_PLAYERS); main gives it
   to the ENet host and the server as its connections */
#define PROFILE_075_MAX_PLAYERS 32
#define PROFILE_REPORT_MAX      5
#define PROFILE_REPORT_LINE     1024
/* the profile, the config file, the data folder, the ports and the masters,
   one fact a line (docs/plan-075-server.md 4.4): main's start-up banner and
   /profile. port is the game's, max_players the cap the server runs with;
   returns the lines written */
int profile_report_lines(uint16_t port, int max_players, char lines[][PROFILE_REPORT_LINE], int max);
void cmd_stats(void* p_server, command_args_t arguments);
void cmd_nextmap(void* p_server, command_args_t arguments);
void cmd_endmatch(void* p_server, command_args_t arguments);
void cmd_crate(void* p_server, command_args_t arguments);
void cmd_night(void* p_server, command_args_t arguments);
void cmd_sky(void* p_server, command_args_t arguments);
void cmd_perk(void* p_server, command_args_t arguments);
void cmd_outfit(void* p_server, command_args_t arguments);
void cmd_map(void* p_server, command_args_t arguments);
void cmd_restart(void* p_server, command_args_t arguments);
void cmd_extend(void* p_server, command_args_t arguments);
void cmd_time(void* p_server, command_args_t arguments);
void cmd_score(void* p_server, command_args_t arguments);
void cmd_toggle_build(void* p_server, command_args_t arguments);
void cmd_toggle_kill(void* p_server, command_args_t arguments);
void cmd_toggle_team_kill(void* p_server, command_args_t arguments);
void cmd_classes(void* p_server, command_args_t arguments);
void cmd_tp(void* p_server, command_args_t arguments);
void cmd_tpc(void* p_server, command_args_t arguments);
void cmd_unban(void* p_server, command_args_t arguments);
void cmd_unban_range(void* p_server, command_args_t arguments);
void cmd_undo_ban(void* p_server, command_args_t arguments);
void cmd_ups(void* p_server, command_args_t arguments);
void cmd_shutdown(void* p_server, command_args_t arguments);

void cmd_addbot(void* p_server, command_args_t arguments);
void cmd_kickbots(void* p_server, command_args_t arguments);
void cmd_colinfo(void* p_server, command_args_t arguments);
void cmd_kickbot(void* p_server, command_args_t arguments);
void cmd_botfill(void* p_server, command_args_t arguments);
void cmd_autofill(void* p_server, command_args_t arguments);
void cmd_bots(void* p_server, command_args_t arguments);

void hostcmd_apply(server_t* server, player_t* requester, uint8_t command, uint8_t arg1, uint8_t arg2);

#endif
