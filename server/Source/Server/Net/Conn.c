// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Net/Conn.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Net/EnetConn.h>
#include <Server/Net/WebRtc.h>
#include <Server/Proto10/NetStats.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <stdio.h>
#include <string.h>

int conn_send_reliable(conn_t* conn, const uint8_t* data, size_t len)
{
    if (conn_send(conn, 0, data, len, 1) != 0) {
        return -1;
    }
    proto10_netstats_add(data, len);
    return 0;
}

int conn_count_from(uint32_t key, const conn_t* except)
{
    return conn_enet_count_from(key, except) + webrtc_count_from(key, except);
}

uint32_t conn_key_of(const char* address)
{
    uint8_t a6[16];
    uint8_t a4[4];
    if (inet_pton(AF_INET, address, a4) == 1) {
        return ((uint32_t) a4[0] << 24) | ((uint32_t) a4[1] << 16) | ((uint32_t) a4[2] << 8) | (uint32_t) a4[3];
    }
    if (inet_pton(AF_INET6, address, a6) != 1) {
        return 0;
    }
    /* FNV-1a over the /64: the addresses of one home share it */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) {
        h = (h ^ a6[i]) * 16777619u;
    }
    return h;
}

int conn_ipv6_prefix(const char* address, char* out, size_t size)
{
    uint8_t a6[16];
    char    text[INET6_ADDRSTRLEN];
    if (address == NULL || inet_pton(AF_INET6, address, a6) != 1) {
        return 0;
    }
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(a6, mapped, sizeof mapped) == 0) {
        return 0;
    }
    memset(a6 + 8, 0, 8);
    if (inet_ntop(AF_INET6, a6, text, sizeof text) == NULL) {
        return 0;
    }
    snprintf(out, size, "%s/64", text);
    return 1;
}

const char* conn_player_prefix(const struct player* player, char* out, size_t size)
{
    if (player == NULL || player->conn == NULL || player->conn->family != 6) {
        return NULL;
    }
    return conn_ipv6_prefix(player->conn->address, out, size) ? out : NULL;
}
