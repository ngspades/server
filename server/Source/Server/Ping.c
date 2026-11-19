// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Plugins.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/Server.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <enet/enet.h>
#include <json-c/json.h>
#include <json-c/json_object.h>
#include <netaddr/netaddr.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the server list's details page asks a server for what the list does not
   carry (HELLOINFO): the protocol revision, the teams and their score, the
   players, the mode's mechanics, the rotation and the plugins. A 0.75
   server does not answer, and the page shows the list's row alone */
#define INFO_PLAYERS 32 /* the best by kills: a datagram stays a few KB */

static int _by_kills(const void* a, const void* b)
{
    const player_t* pa = *(player_t* const*) a;
    const player_t* pb = *(player_t* const*) b;
    return (int) pb->kills - (int) pa->kills;
}

json_object* ping_info(server_t* server)
{
    json_object* o = json_object_new_object();
    char         version[16];
    snprintf(version, sizeof version, "%d.%d", P10_VERSION_MAJOR, P10_VERSION_MINOR);
    /* the 0.75 profile speaks 0.75 (our master probes HELLOINFO before it
       lists a server, so that profile answers too) */
    if (profile_is_075()) {
        snprintf(version, sizeof version, "0.75");
    }
    const gamemode_ops_t* ops = gamemode_ops_of((uint8_t) server->protocol.current_gamemode);
    json_object_object_add(o, "kind", json_object_new_string("info"));
    json_object_object_add(o, "name", json_object_new_string(server->server_name));
    json_object_object_add(o, "map", json_object_new_string(server->map_name));
    /* the map's shape (common/maphash): the list matches the same map under
       its other names by it */
    json_object_object_add(o, "map_shape", json_object_new_string(server->map_shape));
    /* the top view's encoded bytes, 0 when the map has none: the details
       page knows a server has a picture before it asks for it */
    json_object_object_add(o, "map_preview", json_object_new_int((int) server->map_preview_len));
    json_object_object_add(o, "game_mode", json_object_new_string(ops ? ops->name : server->gamemode_name));
    json_object_object_add(o, "protocol", json_object_new_string(version));
    json_object_object_add(o, "package", json_object_new_string(GAME_SERVER_TITLE));
    json_object_object_add(o, "players_max", json_object_new_int(server->protocol.max_players));

    player_t* list[256];
    int       n = 0, bots = 0, humans = 0, team_players[3] = {0, 0, 0};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot) {
            bots++;
        } else {
            humans++;
        }
        team_players[p->team < 2 ? p->team : 2]++;
        if (n < 256) {
            list[n++] = p;
        }
    }
    json_object_object_add(o, "players_current", json_object_new_int(bots + humans));
    json_object_object_add(o, "bots", json_object_new_int(bots));
    json_object_object_add(o, "humans", json_object_new_int(humans));

    json_object* teams = json_object_new_array();
    for (int t = 0; t < 2; t++) {
        json_object* team = json_object_new_object();
        json_object_object_add(team, "name", json_object_new_string(server->protocol.name_team[t]));
        json_object_object_add(team, "score", json_object_new_int(server->protocol.gamemode.score[t]));
        json_object_object_add(team, "players", json_object_new_int(team_players[t]));
        json_object_array_add(teams, team);
    }
    json_object_object_add(o, "teams", teams);
    json_object_object_add(o, "score_limit", json_object_new_int(server->protocol.gamemode.score_limit));

    json_object* features = json_object_new_object();
    json_object_object_add(features, "perks", json_object_new_boolean(gamemode_has_perks(server)));
    json_object_object_add(features, "classes", json_object_new_boolean(classes_on(server)));
    json_object_object_add(features, "assists", json_object_new_boolean(gamemode_has(server, GM_FEAT_ASSISTS)));
    json_object_object_add(features, "spotting", json_object_new_boolean(gamemode_has(server, GM_FEAT_SPOTTING)));
    json_object_object_add(features, "squad_spawn",
                           json_object_new_boolean(gamemode_has(server, GM_FEAT_SQUAD_SPAWN)));
    json_object_object_add(features, "aim_assist", json_object_new_boolean(server->aim_assist));
    json_object_object_add(o, "features", features);

    if (classes_on(server)) {
        json_object* classes = json_object_new_array();
        for (int k = 0; game_class_get(k) != NULL; k++) {
            if (game_class_get(k)->enabled) {
                json_object_array_add(classes, json_object_new_string(game_class_get(k)->title));
            }
        }
        json_object_object_add(o, "classes", classes);
    }

    json_object*   rotation = json_object_new_array();
    string_node_t* m        = server->s_map.map_list;
    for (int k = 0; m != NULL && k < 32; m = m->next, k++) {
        json_object_array_add(rotation, json_object_new_string(m->string));
    }
    json_object_object_add(o, "rotation", rotation);

    char plugins[512];
    plugins_names(plugins, sizeof plugins);
    json_object_object_add(o, "plugins", json_object_new_string(plugins));

    qsort(list, (size_t) n, sizeof *list, _by_kills);
    json_object* players = json_object_new_array();
    for (int k = 0; k < n && k < INFO_PLAYERS; k++) {
        json_object* pl = json_object_new_object();
        json_object_object_add(pl, "name", json_object_new_string(list[k]->name));
        json_object_object_add(pl, "team", json_object_new_int(list[k]->team));
        json_object_object_add(pl, "kills", json_object_new_int(list[k]->kills));
        json_object_object_add(pl, "deaths", json_object_new_int(list[k]->deaths));
        json_object_object_add(pl, "bot", json_object_new_boolean(list[k]->is_bot));
        json_object_object_add(pl, "ping",
                               json_object_new_int((int) conn_rtt(list[k]->conn)));
        json_object_array_add(players, pl);
    }
    json_object_object_add(o, "players", players);
    return o;
}

/* UDP proves nobody's address, so a query is also a way to make this
   server send its answer to a victim whose address was forged as the
   source (a reflection attack, the kind that gets a server suspended by its
   host). HELLOINFO's answer is several KB: the client pads its query to
   QUERY_PAD bytes, so a forged one amplifies nothing, and answers are
   rate limited per address and in total. The bare 9 byte query (our
   master's probe, older clients) is still answered, under a much tighter
   rate: a forged victim gets a few KB a second at most. HELLOLAN only
   answers the local network. Refusals are counted and logged once a
   second: a flood would otherwise flood the log too */
#define QUERY_PAD          1200
#define QUERY_SLOTS        1024 /* addresses tracked at once; a collision only shares a budget */
#define QUERY_PADDED_RATE  5.0  /* answers a second to one address, padded queries */
#define QUERY_PADDED_BURST 10.0
#define QUERY_BARE_RATE    0.5 /* the short queries: one every two seconds, three at once */
#define QUERY_BARE_BURST   3.0
/* the map preview's chunks: 40 a second bring a 10-30 KB picture in about a
   second, and a forged address gets 40 datagrams a second of answers no
   bigger than its own queries */
#define QUERY_PREVIEW_RATE  40.0
#define QUERY_PREVIEW_BURST 40.0
#define QUERY_TOTAL_RATE   200.0 /* every answer of this server, a second */
#define QUERY_TOTAL_BURST  400.0

/* what a query spends: the short ones get a much tighter budget than the
   padded ones, and a preview's chunks one of their own */
enum
{
    QUERY_BARE,
    QUERY_PADDED,
    QUERY_PREVIEW
};

typedef struct
{
    ENetAddress addr;
    double      padded, bare, preview; /* tokens */
    uint64_t    at;
} query_slot_t;

static query_slot_t query_slots[QUERY_SLOTS];
static double       query_total = QUERY_TOTAL_BURST;
static uint64_t     query_total_at;

static double _refill(double tokens, uint64_t* at, uint64_t now, double rate, double burst)
{
    double t = tokens + (double) (now - *at) / 1e9 * rate;
    *at      = now;
    return t > burst ? burst : t;
}

static uint32_t _addr_hash(const ENetAddress* a)
{
    uint32_t       h = 2166136261u; /* FNV-1a over the host's bytes */
    const uint8_t* b = (const uint8_t*) &a->host;
    for (size_t i = 0; i < sizeof a->host; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* takes one answer from the address's budget and the server's; 0 when
   either is spent */
static int _query_allowed(const ENetAddress* from, int kind)
{
    uint64_t    now = get_real_nanos();
    ENetAddress a   = *from;
    netaddr_unmap(&a);
    a.port            = 0;
    query_slot_t* slot = &query_slots[_addr_hash(&a) % QUERY_SLOTS];
    if (slot->at == 0 || !enet_address_equal_host(&slot->addr, &a)) {
        slot->addr    = a;
        slot->padded  = QUERY_PADDED_BURST;
        slot->bare    = QUERY_BARE_BURST;
        slot->preview = QUERY_PREVIEW_BURST;
        slot->at      = now;
    }
    uint64_t at_padded = slot->at, at_bare = slot->at, at_preview = slot->at;
    slot->padded  = _refill(slot->padded, &at_padded, now, QUERY_PADDED_RATE, QUERY_PADDED_BURST);
    slot->bare    = _refill(slot->bare, &at_bare, now, QUERY_BARE_RATE, QUERY_BARE_BURST);
    slot->preview = _refill(slot->preview, &at_preview, now, QUERY_PREVIEW_RATE, QUERY_PREVIEW_BURST);
    slot->at      = now;
    if (query_total_at == 0) {
        query_total_at = now;
    }
    query_total  = _refill(query_total, &query_total_at, now, QUERY_TOTAL_RATE, QUERY_TOTAL_BURST);
    double* mine = kind == QUERY_BARE ? &slot->bare : kind == QUERY_PREVIEW ? &slot->preview : &slot->padded;
    if (*mine < 1.0 || query_total < 1.0) {
        return 0;
    }
    *mine -= 1.0;
    query_total -= 1.0;
    return 1;
}

static void _query_refused(const char* why)
{
    static uint64_t last;
    static unsigned refused;
    uint64_t        now = get_real_nanos();
    refused++;
    if (now - last >= 1000000000ull) {
        evt_log(NULL, "net", "action=query_refused why=%s count=%u", why, refused);
        refused = 0;
        last    = now;
    }
}

/* a query is its word, then (HELLOINFO) padding to QUERY_PAD */
static int _is_query(const ENetHost* host, const char* word, size_t* padded)
{
    size_t n = strlen(word);
    if (host->receivedDataLength < n || memcmp(host->receivedData, word, n)) {
        return 0;
    }
    *padded = host->receivedDataLength >= QUERY_PAD;
    return host->receivedDataLength == n || *padded;
}

int raw_udp_intercept_callback(ENetHost* host, ENetEvent* event)
{
    (void) event;
    size_t padded = 0;
    /* the whole word: compared over the bytes that came, a packet "H" was
       an "HELLO". A longer HELLOINFO is the padded one */
    if (_is_query(host, "HELLOINFO", &padded)) {
        server_t* server = get_server();
        if (strlen(server->map_name) == 0) {
            return 1;
        }
        if (!_query_allowed(&host->receivedAddress, padded ? QUERY_PADDED : QUERY_BARE)) {
            _query_refused(padded ? "rate" : "rate_short");
            return 1;
        }
        json_object* info = ping_info(server);
        const char*  json = json_object_to_json_string(info);
        enet_socket_send(host->socket, &host->receivedAddress,
                         &(ENetBuffer){.data = (char*) json, .dataLength = strlen(json)}, 1);
        json_object_put(info);
        return 1;
    } else if (host->receivedDataLength == 5 && !memcmp(host->receivedData, "HELLO", 5)) {
        /* 2 bytes for 5: nothing to amplify, but a flood still costs sends */
        if (!_query_allowed(&host->receivedAddress, QUERY_PADDED)) {
            _query_refused("rate");
            return 1;
        }
        enet_socket_send(host->socket, &host->receivedAddress, &(ENetBuffer){.data = "HI", .dataLength = 2}, 1);
        return 1;
    } else if (host->receivedDataLength == 8 && !memcmp(host->receivedData, "HELLOLAN", 8)) {
        server_t* server = get_server();
        /* the LAN list's question: nobody on the internet needs its answer */
        if (!netaddr_is_local(&host->receivedAddress)) {
            _query_refused("not_lan");
            return 1;
        }
        if (!_query_allowed(&host->receivedAddress, QUERY_PADDED)) {
            _query_refused("rate");
            return 1;
        }
        /* answer even before the map is fully loaded: a server that is
           still starting up must be discoverable on the LAN */
        const char* lan_map = server->map_name[0] != '\0' ? server->map_name : "unknown";
        json_object* jobj                  = json_object_new_object();
        json_object* serverNameObject      = json_object_new_string(server->server_name);
        json_object* mapNameObject         = json_object_new_string(lan_map);
        json_object* gamemodeObject        = json_object_new_string(server->gamemode_name);
        /* the only one it speaks: the profile's */
        json_object* protocolVersionObject = json_object_new_string(profile_is_075() ? "0.75" : "1.0");
        /* everyone playing, bots too (num_users stayed 0 with a host and 8 bots) */
        json_object* currentPlayersObject  = json_object_new_int(server->protocol.num_players);
        json_object* maxPlayersObject      = json_object_new_int(server->protocol.max_players);

        json_object_object_add(jobj, "name", serverNameObject);
        json_object_object_add(jobj, "players_current", currentPlayersObject);
        json_object_object_add(jobj, "players_max", maxPlayersObject);
        json_object_object_add(jobj, "map", mapNameObject);
        json_object_object_add(jobj, "map_shape", json_object_new_string(server->map_shape));
        json_object_object_add(jobj, "game_mode", gamemodeObject);
        json_object_object_add(jobj, "game_version", protocolVersionObject);

        const char* json = json_object_to_json_string(jobj);

        evt_log(NULL, "lan", "action=answer name=%s map=%s mode=%s players=%d/%d",
                server->server_name, lan_map, server->gamemode_name,
                server->protocol.num_players, server->protocol.max_players);

        enet_socket_send(
        host->socket, &host->receivedAddress, &(ENetBuffer){.data = (char*) json, .dataLength = strlen(json)}, 1);

        while (json_object_put(jobj) != 1) {
            // Free the memory from the JSON object
        }

        return 1;
    } else if (host->receivedDataLength >= 12 && !memcmp(host->receivedData, "HELLOPREVIEW", 12)) {
        /* The map preview's chunks (MapPreview.c): a padded query asking an
           offset, answered with at most as many bytes as the query itself
           carried, so a forged source amplifies nothing. A preview of about
           10-30 KB comes in a second at this budget. */
        server_t* server = get_server();
        if (host->receivedDataLength < QUERY_PAD) {
            _query_refused("preview_short");
            return 1;
        }
        if (!_query_allowed(&host->receivedAddress, QUERY_PREVIEW)) {
            _query_refused("rate_preview");
            return 1;
        }
        const uint8_t* q      = host->receivedData;
        uint32_t       offset = (uint32_t) q[12] | (uint32_t) q[13] << 8 | (uint32_t) q[14] << 16 |
                                (uint32_t) q[15] << 24;
        uint32_t total = (uint32_t) server->map_preview_len;
        uint8_t  answer[QUERY_PAD];
        size_t   at = 0;
        memcpy(answer, "SDPC", 4);
        at = 4;
        for (int k = 0; k < 4; k++) {
            answer[at++] = (uint8_t) ((total >> (8 * k)) & 0xFF);
        }
        for (int k = 0; k < 4; k++) {
            answer[at++] = (uint8_t) ((offset >> (8 * k)) & 0xFF);
        }
        /* 64 hex characters, no NUL: the client files the picture by the
           shape, which is the same however a file was saved or renamed */
        memset(answer + at, 0, 64);
        if (server->map_shape[0] != '\0') {
            memcpy(answer + at, server->map_shape, 64);
        }
        at += 64;
        size_t limit = host->receivedDataLength < QUERY_PAD ? host->receivedDataLength : QUERY_PAD;
        if (total > 0 && offset < total && server->map_preview != NULL) {
            size_t take = (size_t) total - (size_t) offset;
            if (take > limit - at) {
                take = limit - at;
            }
            memcpy(answer + at, server->map_preview + offset, take);
            at += take;
        }
        enet_socket_send(host->socket, &host->receivedAddress, &(ENetBuffer){.data = (char*) answer, .dataLength = at},
                         1);
        return 1;
    }
    return 0;
}
