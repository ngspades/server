// Copyright DarkNeutrino 2021
// Copyright (c) 2026 ngspades contributors
/*
 * The announce to the public 0.75 masters (docs/plan-075-server.md 8.6),
 * restored from SpadesX's Master.c (ba72777^) and driven from the tick
 * instead of a thread of its own: ENet may only be driven from one thread,
 * and the old keep-alive thread slept 30 s holding the server's lock when
 * the master went away.
 *
 * The classic master's wire (pyspades' master.py, the one the vanilla
 * clients' list comes from): an ENet connection with connect data 31 (the
 * master protocol's version), then one reliable packet
 *   u8 max_players, u16 port (little endian), name\0, game mode\0, map\0
 * and afterwards a one byte packet with the player count whenever it
 * changes. The master lists the address the connection came from.
 *
 * The ENet host is our own, apart from the game's: a master's packets must
 * never reach the game's receive path, and the game's host must never see
 * an outgoing connection.
 */
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Legacy/Master075.h>
#include <Server/Profile.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <enet/enet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M075_DEFAULT_ADDRESS "67.205.183.163:32886"
#define M075_DEFAULT_PORT    32886
#define M075_VERSION         31 /* the master protocol's version, the connect data */
#define M075_MAX_PLAYERS     32 /* the classic limit the list shows */
#define M075_MAX_MASTERS     4
/* a lost or refused master is tried again after this (the old code's 30 s) */
#define M075_RETRY_NS (30ULL * NANO_IN_SECOND)
/* the player count goes out at most once a second (joins come in bursts) */
#define M075_COUNT_NS (1ULL * NANO_IN_SECOND)
/* a name, map or mode change is announced again (a new connection), at
   most this often */
#define M075_CHANGE_NS (5ULL * NANO_IN_SECOND)

typedef enum m075_state {
    M075_IDLE,       /* not connected: retry_at says when to try */
    M075_CONNECTING, /* enet_host_connect sent, waiting for its CONNECT */
    M075_LISTED,     /* the major packet went: count updates only */
    M075_LEAVING,    /* disconnecting to announce again (a change) */
    M075_DEAD,       /* the address does not resolve: never tried again */
} m075_state_t;

typedef struct m075_master
{
    char         address[128]; /* as configured, for the logs */
    ENetAddress  enet;
    m075_state_t state;
    ENetPeer*    peer;
    uint64_t     retry_at;
    uint64_t     count_at;
    uint64_t     listed_at;
    int          count_sent;              /* -1 none */
    char         announced[128];          /* name|map|mode it was listed with */
} m075_master_t;

static struct
{
    int           enabled;
    int           on;
    int           count;
    m075_master_t masters[M075_MAX_MASTERS];
    ENetHost*     host;
} m075 = {.enabled = 1};

static char m075_config_addresses[M075_MAX_MASTERS][128];
static int  m075_config_count = -1; /* -1: [master075] address not given */

/* ---- configuration ---- */

void master075_configure(toml_table_t* root)
{
    m075.enabled      = 1;
    m075_config_count = -1;
    toml_table_t* t   = root != NULL ? toml_table_in(root, "master075") : NULL;
    if (t == NULL) {
        return;
    }
    toml_datum_t d = toml_bool_in(t, "enabled");
    if (d.ok) {
        m075.enabled = d.u.b;
    }
    /* one master or a list of them */
    d = toml_string_in(t, "address");
    if (d.ok) {
        snprintf(m075_config_addresses[0], sizeof m075_config_addresses[0], "%s", d.u.s);
        free(d.u.s);
        m075_config_count = 1;
        return;
    }
    toml_array_t* list = toml_array_in(t, "address");
    if (list != NULL) {
        m075_config_count = 0;
        for (int i = 0; i < toml_array_nelem(list); i++) {
            toml_datum_t s = toml_string_at(list, i);
            if (!s.ok) {
                continue;
            }
            if (m075_config_count < M075_MAX_MASTERS) {
                snprintf(m075_config_addresses[m075_config_count], sizeof m075_config_addresses[0], "%s", s.u.s);
                m075_config_count++;
            } else {
                LOG_WARNING("master075: more than %d addresses, %s is not announced to", M075_MAX_MASTERS, s.u.s);
            }
            free(s.u.s);
        }
    }
}

/* "host:port" (port optional) into an ENet address; 0 when it does not
   resolve. A name is resolved once, here, never on the tick again. */
static int _m075_resolve(const char* text, ENetAddress* out)
{
    char        host[128];
    long        port  = M075_DEFAULT_PORT;
    const char* colon = strrchr(text, ':');
    size_t      len   = colon != NULL ? (size_t) (colon - text) : strlen(text);
    if (len == 0 || len >= sizeof host) {
        return 0;
    }
    memcpy(host, text, len);
    host[len] = '\0';
    if (colon != NULL) {
        char* end;
        port = strtol(colon + 1, &end, 10);
        if (*end != '\0' || port <= 0 || port > 65535) {
            return 0;
        }
    }
    memset(out, 0, sizeof *out);
    if (enet_address_set_host(out, ENET_ADDRESS_TYPE_IPV4, host) != 0) {
        return 0;
    }
    out->port = (enet_uint16) port;
    return 1;
}

/* ---- what is announced ---- */

static const char* _m075_mode(server_t* server)
{
    const gamemode_ops_t* ops = gamemode_ops_of((uint8_t) server->protocol.current_gamemode);
    return ops != NULL ? ops->name : server->gamemode_name;
}

static int _m075_players(server_t* server)
{
    /* bots count, as on our own list and in HELLOLAN */
    int n = server->protocol.num_players;
    return n > M075_MAX_PLAYERS ? M075_MAX_PLAYERS : n;
}

static void _m075_listing(server_t* server, char* out, size_t size)
{
    snprintf(out, size, "%s|%s|%s", server->server_name, server->map_name, _m075_mode(server));
}

static void _m075_send_major(server_t* server, m075_master_t* m)
{
    const char* name = server->server_name;
    const char* mode = _m075_mode(server);
    const char* map  = server->map_name;
    size_t      ln = strlen(name) + 1, lm = strlen(mode) + 1, lp = strlen(map) + 1;
    ENetPacket* packet = enet_packet_create(NULL, 3 + ln + lm + lp, ENET_PACKET_FLAG_RELIABLE);
    if (packet == NULL) {
        return;
    }
    uint8_t* d = packet->data;
    d[0]       = M075_MAX_PLAYERS;
    d[1]       = (uint8_t) (server->port & 0xFF);
    d[2]       = (uint8_t) (server->port >> 8);
    memcpy(d + 3, name, ln);
    memcpy(d + 3 + ln, mode, lm);
    memcpy(d + 3 + ln + lm, map, lp);
    if (enet_peer_send(m->peer, 0, packet) != 0) {
        enet_packet_destroy(packet);
        return;
    }
    _m075_listing(server, m->announced, sizeof m->announced);
    m->state      = M075_LISTED;
    m->listed_at  = get_real_nanos();
    m->count_sent = -1; /* the count follows at once */
    LOG_STATUS("Announced to the 0.75 master %s as '%s' (%s on %s, port %u)", m->address, name, mode, map,
               (unsigned) server->port);
    evt_log(NULL, "master075", "action=listed master=%s name=\"%s\" map=%s mode=%s port=%u max=%d", m->address, name,
            map, mode, (unsigned) server->port, M075_MAX_PLAYERS);
}

static void _m075_send_count(server_t* server, m075_master_t* m, uint64_t now)
{
    int n = _m075_players(server);
    if (n == m->count_sent || now < m->count_at) {
        return;
    }
    ENetPacket* packet = enet_packet_create(NULL, 1, ENET_PACKET_FLAG_RELIABLE);
    if (packet == NULL) {
        return;
    }
    packet->data[0] = (uint8_t) n;
    if (enet_peer_send(m->peer, 0, packet) != 0) {
        enet_packet_destroy(packet);
        return;
    }
    m->count_sent = n;
    m->count_at   = now + M075_COUNT_NS;
    evt_log(NULL, "master075", "action=count master=%s players=%d", m->address, n);
}

/* ---- the connections ---- */

static void _m075_connect(m075_master_t* m)
{
    m->peer = enet_host_connect(m075.host, &m->enet, 2, M075_VERSION);
    if (m->peer == NULL) {
        m->state    = M075_IDLE;
        m->retry_at = get_real_nanos() + M075_RETRY_NS;
        evt_log(NULL, "master075", "action=error master=%s why=no_peer", m->address);
        return;
    }

    m->state      = M075_CONNECTING;
    evt_log(NULL, "master075", "action=connect master=%s", m->address);
}

static m075_master_t* _m075_of(ENetPeer* peer)
{
    for (int i = 0; i < m075.count; i++) {
        if (m075.masters[i].peer == peer) {
            return &m075.masters[i];
        }
    }
    return NULL;
}

void master075_start(server_t* server)
{
    if (!profile_is_075() || m075.on) {
        return;
    }
    const char* env = getenv("SP_MASTER");
    if (env != NULL && !strcmp(env, "0")) {
        LOG_INFO("master075: not announced to the 0.75 masters (SP_MASTER=0)");
        return;
    }
    if (!m075.enabled) {
        LOG_INFO("master075: not announced to the 0.75 masters ([master075] enabled = false)");
        return;
    }
    /* a player's own match from the menu stays off every list, as on ours */
    if (server->listen_private) {
        LOG_INFO("master075: not announced to the 0.75 masters (a listen server; --public lists it)");
        return;
    }
    int n = m075_config_count;
    if (n < 0) {
        snprintf(m075_config_addresses[0], sizeof m075_config_addresses[0], "%s", M075_DEFAULT_ADDRESS);
        n = 1;
    }
    m075.count = 0;
    for (int i = 0; i < n; i++) {
        m075_master_t* m = &m075.masters[m075.count];
        memset(m, 0, sizeof *m);
        snprintf(m->address, sizeof m->address, "%s", m075_config_addresses[i]);
        if (!_m075_resolve(m->address, &m->enet)) {
            LOG_WARNING("master075: %s is not host:port of a reachable name: not announced there", m->address);
            evt_log(NULL, "master075", "action=error master=%s why=address", m->address);
            continue;
        }
        m->state      = M075_IDLE;
        m->count_sent = -1;
        m075.count++;
    }
    if (m075.count == 0) {
        return;
    }
    m075.host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, NULL, (size_t) m075.count, 2, 0, 0);
    if (m075.host == NULL) {
        LOG_WARNING("master075: could not create an ENet host: not announced to the 0.75 masters");
        m075.count = 0;
        return;
    }
    /* the classic master compresses as the game does */
    enet_host_compress_with_range_coder(m075.host);
    m075.on = 1;
    for (int i = 0; i < m075.count; i++) {
        _m075_connect(&m075.masters[i]);
    }
}

void master075_tick(server_t* server)
{
    if (!m075.on) {
        return;
    }
    ENetEvent ev;
    while (enet_host_service(m075.host, &ev, 0) > 0) {
        m075_master_t* m = _m075_of(ev.peer);
        if (m == NULL) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
            continue;
        }
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                evt_log(NULL, "master075", "action=connected master=%s", m->address);
                if (server->map_name[0] != '\0') {
                    _m075_send_major(server, m);
                }
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: /* enet6 reports a timeout apart; the game treats it as a disconnect */
            {
                int again = m->state == M075_LEAVING;
                if (!again) {
                    /* refused, timed out or lost */
                    if (m->state == M075_LISTED) {
                        LOG_WARNING("master075: connection to %s lost, trying again in %llu s", m->address,
                                    (unsigned long long) (M075_RETRY_NS / NANO_IN_SECOND));
                    }
                    evt_log(NULL, "master075", "action=error master=%s why=%s data=%u", m->address,
                            m->state == M075_LISTED ? "lost" : "no_answer", (unsigned) ev.data);
                }
                m->peer     = NULL;
                m->state    = M075_IDLE;
                m->retry_at = again ? 0 : get_real_nanos() + M075_RETRY_NS;
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE:
                /* the master says nothing the server needs */
                enet_packet_destroy(ev.packet);
                break;
            default:
                break;
        }
    }

    uint64_t now = get_real_nanos();
    char     listing[128];
    _m075_listing(server, listing, sizeof listing);
    for (int i = 0; i < m075.count; i++) {
        m075_master_t* m = &m075.masters[i];
        switch (m->state) {
            case M075_IDLE:
                if (now >= m->retry_at) {
                    _m075_connect(m);
                }
                break;
            case M075_CONNECTING:
                /* connected while between maps: list once a map is up */
                if (m->peer != NULL && m->peer->state == ENET_PEER_STATE_CONNECTED && server->map_name[0] != '\0') {
                    _m075_send_major(server, m);
                }
                break;
            case M075_LISTED:
                if (server->map_name[0] == '\0') {
                    break; /* between maps */
                }
                /* the major packet is the master's only record of the name,
                   map and mode: a change is a new connection */
                if (strcmp(listing, m->announced) != 0 && now - m->listed_at >= M075_CHANGE_NS) {
                    evt_log(NULL, "master075", "action=reannounce master=%s was=\"%s\" now=\"%s\"", m->address,
                            m->announced, listing);
                    enet_peer_disconnect(m->peer, 0);
                    m->state = M075_LEAVING;
                    break;
                }
                _m075_send_count(server, m, now);
                break;
            default:
                break;
        }
    }
}

void master075_stop(server_t* server)
{
    (void) server;
    if (!m075.on) {
        return;
    }
    int waiting = 0;
    for (int i = 0; i < m075.count; i++) {
        m075_master_t* m = &m075.masters[i];
        if (m->peer != NULL) {
            if (m->state == M075_LISTED || m->state == M075_LEAVING) {
                enet_peer_disconnect(m->peer, 0);
                waiting++;
            } else {
                enet_peer_reset(m->peer);
                m->peer = NULL;
            }
        }
    }
    /* the goodbye must leave before the process does: half a second at most */
    uint64_t  until = get_real_nanos() + NANO_IN_SECOND / 2;
    ENetEvent ev;
    while (waiting > 0 && get_real_nanos() < until) {
        if (enet_host_service(m075.host, &ev, 20) <= 0) {
            continue;
        }
        m075_master_t* m = _m075_of(ev.peer);
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(ev.packet);
        } else if ((ev.type == ENET_EVENT_TYPE_DISCONNECT || ev.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT) && m != NULL) {
            evt_log(NULL, "master075", "action=left master=%s", m->address);
            m->peer = NULL;
            waiting--;
        }
    }
    for (int i = 0; i < m075.count; i++) {
        if (m075.masters[i].peer != NULL) {
            enet_peer_reset(m075.masters[i].peer);
            m075.masters[i].peer = NULL;
        }
    }
    enet_host_destroy(m075.host);
    m075.host  = NULL;
    m075.on    = 0;
    m075.count = 0;
}
