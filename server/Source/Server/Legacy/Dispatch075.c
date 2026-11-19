// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The 0.75 receive table (docs/plan-075-server.md 8.1): init_packets and
 * on_packet_received of ba72777^'s Packets/PacketManager.c, the same 18
 * handlers (they are the gameplay API the 1.0 bridge replays into, shared by
 * both profiles), plus the extensions' (Ext075.c): 60, the client's ExtInfo
 * answer, 112, its Teamplay packet, 114, its Flashlight request, and the
 * Extra Packets' 32 (the handshake's answer) and 9 marked 'x' (the extended
 * version answer). A chat line whose text starts with 0xFF is UTF-8 and is
 * handed on as the CP437 the server keeps.
 *
 * Every handler gets the stream at the byte after the packet id, as the old
 * dispatcher handed it over.
 */
#include <Server/Legacy/Wire075.h>

#include <Server/EventLog.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Packets/ReceivePackets.h>
#include <Util/DataStream.h>
#include <Util/Log.h>

#include <string.h>

typedef void (*legacy_handler_t)(server_t* server, player_t* player, stream_t* data);

static legacy_handler_t legacy_table[256];
static uint8_t          legacy_table_ready;
static unsigned         legacy_unknown_warned;

/* ExistingPlayer (9), unless it is the extended version answer: its first
   byte is 'x', which no 0.75 player id (at most 31) can be */
static void _existing_or_version(server_t* server, player_t* player, stream_t* data)
{
    if (stream_left(data) >= 1 && data->data[data->pos] == EXT075_VERSION_EXT_MARKER) {
        ext075_receive_version_ext(server, player, data);
        return;
    }
    receive_existing_player(server, player, data);
}

/* the plain version answer, then the Extra Packets' questions */
static void _version_response(server_t* server, player_t* player, stream_t* data)
{
    receive_version_response(server, player, data);
    ext075_on_version(server, player);
}

/* 0.76's MapCached: the 0.75 Map Start carries no CRC, the map always goes */
static void _map_cached(server_t* server, player_t* player, stream_t* data)
{
    (void) server;
    (void) data;
    evt_log(player, "wire", "profile=075 action=map_cached_ignored");
}

/* Chat (17): a text starting with 0xFF is UTF-8 (the UTF-8 chat
   extension); the server's chat handler and commands read CP437 */
static void _chat(server_t* server, player_t* player, stream_t* data)
{
    if (stream_left(data) < 3 || data->data[data->pos + 2] != 0xFF) {
        receive_handle_send_message(server, player, data);
        return;
    }
    uint8_t        line[2 + 1024];
    const uint8_t* at  = data->data + data->pos;
    size_t         len = stream_left(data);
    line[0]            = at[0];
    line[1]            = at[1];
    ext075_chat_utf8_begin(player, at + 3, len - 3, (char*) line + 2, sizeof line - 2);
    stream_t s = {line, (uint32_t) (2 + strlen((char*) line + 2) + 1), 0};
    receive_handle_send_message(server, player, &s);
    ext075_chat_utf8_done();
}

static void _init_table(void)
{
    legacy_table[0]   = receive_position_data;
    legacy_table[1]   = receive_orientation_data;
    legacy_table[3]   = receive_input_data;
    legacy_table[4]   = receive_weapon_input;
    legacy_table[5]   = receive_hit_packet;
    legacy_table[6]   = receive_grenade_packet;
    legacy_table[7]   = receive_set_tool;
    legacy_table[8]   = receive_set_color;
    legacy_table[9]   = _existing_or_version;
    legacy_table[10]  = receive_short_player;
    legacy_table[13]  = receive_block_action;
    legacy_table[14]  = receive_block_line;
    legacy_table[17]  = _chat;
    legacy_table[28]  = receive_weapon_reload;
    legacy_table[29]  = receive_change_team;
    legacy_table[30]  = receive_change_weapon;
    legacy_table[31]  = _map_cached;
    legacy_table[EXT075_PACKET_HANDSHAKE_RESPONSE] = ext075_receive_handshake;
    legacy_table[34]  = _version_response;
    legacy_table[63]  = receive_host_command;
    legacy_table[60]  = ext075_receive_info;
    legacy_table[112] = ext075_receive_teamplay;
    legacy_table[EXT075_PACKET_FLASHLIGHT] = ext075_receive_flashlight;
    legacy_table_ready = 1;
}

void on_packet_received_075(server_t* server, player_t* player, const uint8_t* data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (!legacy_table_ready) {
        _init_table();
    }
    /* the handlers only read; stream_t has no const flavour */
    stream_t stream = {(uint8_t*) data, (uint32_t) len, 0};
    uint8_t  type   = stream_read_u8(&stream);
    if (legacy_table[type] == NULL) {
        if (legacy_unknown_warned < 32) {
            legacy_unknown_warned++;
            LOG_WARNING("Unknown packet with ID %d received", (int) type);
        }
        evt_log(player, "wire", "profile=075 action=unknown_packet id=%u size=%zu", (unsigned) type, len);
        return;
    }
    legacy_table[type](server, player, &stream);
    switch (type) {
        case 4:  /* the clip */
        case 5:  /* the ammo of a hit */
        case 6:  /* a grenade gone */
        case 13: /* blocks */
        case 14:
        case 28:
            ext075_player_changed(server, player);
            break;
        default:
            break;
    }
}
