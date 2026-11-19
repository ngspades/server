#include <Server/Flood.h>
#include <Server/GameApi.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Plugins.h>
#include <Server/Commands/Commands.h>
#include <Server/Proto10/World.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Server/Staff.h>
#include <Util/Alloc.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <ctype.h>
#include <string.h>

/* A chat line: channel 0 global, 1 team, 2 system. A line that starts with
   '/' is a command, checked by the same rules as the console's commands; a
   plugin hears it first (on_chat) and may refuse or rewrite it. The text is
   CP437 here, the server's one internal text. */
void game_player_chat(server_t* server, player_t* player, int meant_for, const char* message)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    /* over the chat budget: dropped before it is read (Flood.c warns,
       then kicks a flood) */
    if (!flood_take(server, player, FLOOD_CHAT)) {
        return;
    }
    if (!diff_is_older_then(get_nanos(), &player->timers.since_last_message, (uint64_t) NANO_IN_MILLI * 400) &&
        player->permissions <= 1)
    {
        send_server_notice_kind(
        player, NOTICE_WARNING, "WARNING: You sent last message too fast and thus was not sent out to players");
        return;
    }

    for (unsigned long index = 0; index < strlen(message); ++index) {
        if (isgraph(message[index]) == 0 && message[index] != ' ' && message[index] > '\b') {
            send_server_notice_kind(
            player, NOTICE_WARNING, "WARNING: The message you sent contained unreadable characters and thus was ignored");
            LOG_WARNING("Player %s (#%hhu) tried to send message containing unreadable character. Message ignored",
                        player->name,
                        player->id);
            return;
        }
    }

    char meantFor[7];
    switch (meant_for) {
        case 0:
            snprintf(meantFor, 7, "Global");
            break;
        case 1:
            snprintf(meantFor, 5, "Team");
            break;
        case 2:
            snprintf(meantFor, 7, "System");
            break;
    }
    LOG_INFO("Player %s (#%hhu) (%s) said: %s", player->name, player->id, meantFor, message);

    if (message[0] == '/') {
        command_handle(server, player, (char*) message, 0);
    } else if (!player->muted) {
        /* a plugin may refuse the line or rewrite it (on_chat), longer too */
        char text[512];
        snprintf(text, sizeof text, "%s", message);
        if (plugins_on_chat(server, player, text, sizeof text, meant_for == 1)) {
            if (profile_is_075()) {
                legacy_send_chat(server, player, (uint8_t) meant_for, text);
            } else {
                proto10_world_chat(server, player, (uint8_t) meant_for, text);
            }
        }
    }
}

/* Chat (17): the player's id, the channel, then the text up to the packet's
   end (at most 2048 characters, a NUL that a client may leave out) */
void receive_handle_send_message(server_t* server, player_t* player, stream_t* data)
{
    uint8_t  received_id = stream_read_u8(data);
    int      meant_for   = stream_read_u8(data);
    uint32_t length      = stream_left(data);
    if (length > 2048) {
        length = 2048; // Lets limit messages to 2048 characters
    }
    /* one more byte, so a client that sent no NUL still gives a string */
    char* message = spadesx_calloc(length + 1, sizeof(char));
    stream_read_array(data, message, length);
    if (length > 0 && message[length - 1] != '\0') {
        message[length] = '\0';
    }
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in message packet", player->id, received_id);
    }
    game_player_chat(server, player, meant_for, message);
    free(message);
}
