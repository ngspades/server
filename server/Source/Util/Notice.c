#include <Server/Legacy/Ext075.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Proto10/World.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Log.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char*  capture_buf  = NULL;
static size_t capture_size = 0;

void notice_capture(char* buf, size_t size)
{
    capture_buf  = buf;
    capture_size = size;
}

static void _captured(const char* line)
{
    if (capture_buf != NULL) {
        size_t used = strlen(capture_buf);
        snprintf(capture_buf + used, capture_size - used, "%s\n", line);
    }
}

static void _send(player_t* player, uint8_t console, notice_kind_t kind, const char* fMessage)
{
    if (console) {
        LOG_INFO("%s", fMessage);
        _captured(fMessage);
        return;
    }

    if (profile_is_075()) {
        legacy_send_notice(player, (uint8_t) kind, fMessage);
        return;
    }
    proto10_world_notice(player, fMessage);
}

void send_server_notice(player_t* player, uint8_t console, const char* message, ...)
{
    /* send_message_to_staff logs through here with player == NULL and
       console == 1: checking the peer first dereferenced NULL and crashed the
       server on every anti-cheat warning (e.g. building fast) */
    if (!console && (player == NULL || player->conn == NULL)) {
        return;
    }
    va_list args;
    va_start(args, message);
    char fMessage[1024];
    vsnprintf(fMessage, 1024, message, args);
    va_end(args);
    _send(player, console, NOTICE_SYSTEM, fMessage);
}

void send_server_notice_kind(player_t* player, notice_kind_t kind, const char* message, ...)
{
    if (player == NULL || player->conn == NULL) {
        return;
    }
    va_list args;
    va_start(args, message);
    char fMessage[1024];
    vsnprintf(fMessage, 1024, message, args);
    va_end(args);
    _send(player, 0, kind, fMessage);
}

static void _broadcast(server_t* server, uint8_t console, notice_kind_t kind, const char* fMessage)
{
    if (console) {
        LOG_INFO("%s", fMessage);
        _captured(fMessage);
    }
    if (profile_is_075()) {
        legacy_broadcast_notice(server, (uint8_t) kind, fMessage);
        return;
    }
    proto10_world_broadcast_notice(server, fMessage);
}

void broadcast_server_notice(server_t* server, uint8_t console, const char* message, ...)
{
    va_list args;
    va_start(args, message);
    char fMessage[1024];
    vsnprintf(fMessage, 1024, message, args);
    va_end(args);
    _broadcast(server, console, NOTICE_SYSTEM, fMessage);
}

void broadcast_server_notice_kind(server_t* server, uint8_t console, notice_kind_t kind, const char* message, ...)
{
    va_list args;
    va_start(args, message);
    char fMessage[1024];
    vsnprintf(fMessage, 1024, message, args);
    va_end(args);
    _broadcast(server, console, kind, fMessage);
}

void notice_kick(player_t* player, uint32_t reason, conn_close_t how, const char* text, ...)
{
    if (player == NULL || player->conn == NULL) {
        return; /* a bot has no connection to close */
    }
    if (text != NULL && profile_is_075()) {
        va_list args;
        va_start(args, text);
        char words[256];
        vsnprintf(words, sizeof words, text, args);
        va_end(args);
        if (ext075_kick_reason(player, words) && how == CONN_CLOSE) {
            how = CONN_CLOSE_LATER;
        }
    }
    conn_disconnect(player->conn, reason, how);
}
