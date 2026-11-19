#include <Server/GameApi.h>
#include <Server/Plugins.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Packets/Packets.h>
#include <Server/Server.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <classes/classes.h>

/* the reference's raiseTime: a gun taken out fires after it, as the client
   waits. A shot's packet may come up to a ping's jitter sooner than the
   client's clock says, so 150 ms early passes. The 0.75 profile keeps its
   rules (its clients count nothing) */
#define RAISE_SLACK_NS (150ULL * NANO_IN_MILLI)

uint8_t player_gun_raised(player_t* player, uint64_t now)
{
    if (profile_is_075() || player->item_at == 0) {
        return 1;
    }
    uint64_t raise = (uint64_t) GAME_WEAPON_RAISE_MS * NANO_IN_MILLI;
    if (now - player->item_at + RAISE_SLACK_NS >= raise) {
        return 1;
    }
    evt_log(player, "shot_refused", "why=raise ms=%llu", (unsigned long long) ((now - player->item_at) / NANO_IN_MILLI));
    return 0;
}

/* The tool in hand. A gun in hand stops a reload; a tool the mode forbids (a
   zombie's gun) is refused with the reason in the log. */
void game_player_set_tool(server_t* server, player_t* player, uint8_t tool)
{
    if (player->item == tool) {
        return;
    }

    /* the mode's limits (a zombie has its hands only: the spade's slot) */
    unsigned rules = gamemode_player_rules(server, player);
    if ((tool == TOOL_GUN && (rules & GM_RULE_NO_GUN)) || (tool == TOOL_GRENADE && (rules & GM_RULE_NO_GRENADE)) ||
        (tool == TOOL_BLOCK && (rules & GM_RULE_NO_BLOCK)))
    {
        evt_log(player, "tool_refused", "tool=%u why=mode", tool);
        return;
    }
    if (!plugins_on_tool(server, player, tool)) {
        evt_log(player, "tool_refused", "tool=%u why=plugin", tool);
        return;
    }
    if (player->reloading && player->item == TOOL_GUN) {
        player->reloading = 0;
        if (profile_is_075()) {
            legacy_send_weapon_reload(server, player, 0);
        }
    }
    player->item    = tool;
    player->item_at = get_nanos();
    if (profile_is_075()) {
        legacy_send_set_tool(server, player, tool);
    }
}

/* SetTool (7): the player's id, then the tool */
void receive_set_tool(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    uint8_t tool        = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in set tool packet", player->id, received_id);
    }
    game_player_set_tool(server, player, tool);
}
