#ifndef NOTICE_H
#define NOTICE_H

#include <Server/Net/Conn.h>
#include <Server/Structs/ServerStruct.h>

/* what kind of server line a notice is. The 0.75 profile sends the kinds
   past SYSTEM as the Message Types extension's chat types (big in the
   middle of the screen, info, warning, error) to the clients that agreed
   to it and as a plain system line to the others; protocol 1.0 has one
   kind of notice */
typedef enum
{
    NOTICE_SYSTEM  = 2,
    NOTICE_BIG     = 3,
    NOTICE_INFO    = 4,
    NOTICE_WARNING = 5,
    NOTICE_ERROR   = 6,
} notice_kind_t;

void send_server_notice(player_t* player, uint8_t console, const char* message, ...);
void broadcast_server_notice(server_t* server, uint8_t console, const char* message, ...);
/* the same, of a kind */
void send_server_notice_kind(player_t* player, notice_kind_t kind, const char* message, ...);
void broadcast_server_notice_kind(server_t* server, uint8_t console, notice_kind_t kind, const char* message, ...);
/* while buf is set, what is printed for the console is also appended to it
   (the remote console's commands answers with it); NULL stops */
void notice_capture(char* buf, size_t size);

/* closes a player's connection with a disconnect reason (REASON_*), first
   telling a 0.75 client why in words when it agreed to the Kick Reason
   extension (text may be NULL: no words). how is conn_disconnect's; a
   CONN_CLOSE becomes CONN_CLOSE_LATER when words went out, because
   ENet's polite goodbye drops what is still queued */
void notice_kick(player_t* player, uint32_t reason, conn_close_t how, const char* text, ...)
__attribute__((format(printf, 4, 5)));

#endif
