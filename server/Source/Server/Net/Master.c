// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Net/Master.h>
#include <Server/Net/WebRtc.h>
#include <Server/Ping.h>
#include <Server/PluginNet.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/DataDir.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <tomlc99/toml.h>

#define MASTER_KEY_FILE "master.key" /* in the data folder; [master] key_file, SP_MASTER_KEY */
/* the master's API (its README): an address unseen for 90 s is dropped */
#define MASTER_PERIOD_NS (30ULL * NANO_IN_SECOND)
/* a change (a player in or out) is announced, at most this often */
#define MASTER_CHANGE_NS (5ULL * NANO_IN_SECOND)
/* a failed poll waits before the next: a master that is down is not
   hammered 60 times a second */
#define MASTER_RETRY_NS (5ULL * NANO_IN_SECOND)
#define MASTER_POLL_TIMEOUT_S 40 /* the master holds a poll up to 25 s */
#define MASTER_075_MAX_PLAYERS 32 /* the 0.75 profile's row, as the public 0.75 list */

static struct
{
    int       on;
    char      url[256];
    char      country[4];
    char      key_file[1100];
    int       families; /* 1 << 0 IPv4, 1 << 1 IPv6: the families to announce over */
    EVP_PKEY* key;
    char      key_b64[64];
    server_t* server;
    struct
    {
        uint32_t call; /* the announce in flight, 0 none */
        uint64_t sent; /* real nanos of the last announce */
        int      listed;
        char     last_error[96];
        char     state[256]; /* what its last announce said, to see a change */
    } family[2]; /* IPv4, IPv6 */
    uint32_t poll_call;
    int      poll_family;
    uint64_t poll_after;
    /* connect tokens (master_token_*): 0 off, 1 from the internet, 2 from
       local addresses too (SP_CONNECT_TOKENS=all, tests on one machine) */
    int           tokens;
    unsigned char token_key[32]; /* the master's, from every announce's answer */
    int           token_key_ok;
    uint64_t      token_key_at; /* real nanos of the last announce that gave it */
} master;

/* ---- the server's key: made once, kept in the data folder ---- */

static int _b64(const unsigned char* in, size_t len, char* out, size_t size)
{
    if (4 * ((len + 2) / 3) + 1 > size) {
        return 0;
    }
    EVP_EncodeBlock((unsigned char*) out, in, (int) len);
    return 1;
}

static EVP_PKEY* _load_or_make_key(void)
{
    EVP_PKEY* key = NULL;
    FILE*     f   = fopen(master.key_file, "r");
    if (f != NULL) {
        key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
        fclose(f);
        if (key == NULL || EVP_PKEY_get_id(key) != EVP_PKEY_ED25519) {
            LOG_ERROR("master: %s is not an Ed25519 key: move it away to make a new one", master.key_file);
            EVP_PKEY_free(key);
            return NULL;
        }
        return key;
    }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (ctx == NULL || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    f = fopen(master.key_file, "w");
    if (f == NULL || !PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL)) {
        if (f != NULL) {
            fclose(f);
        }
        LOG_ERROR("master: cannot write %s", master.key_file);
        EVP_PKEY_free(key);
        return NULL;
    }
    fclose(f);
    LOG_INFO("master: made the server's key (%s): it is this server's identity on the list", master.key_file);
    return key;
}

/* ---- signed calls ---- */

static uint64_t _unix_ms(void)
{
    /* the wall clock, not the game's: the master compares it with its own */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* {"key","body","sig"} around body (which gets its time), POSTed on lane;
   the call's id, 0 when it could not be made */
static uint32_t _post(net_lane_t lane, const char* path, json_object* body, int family, int timeout_s)
{
    json_object_object_add(body, "time", json_object_new_int64((int64_t) _unix_ms()));
    const char* text = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    unsigned char sig[64];
    size_t        sig_len = sizeof sig;
    EVP_MD_CTX*   md      = EVP_MD_CTX_new();
    int           ok      = md != NULL && EVP_DigestSignInit(md, NULL, NULL, NULL, master.key) > 0 &&
                 EVP_DigestSign(md, sig, &sig_len, (const unsigned char*) text, strlen(text)) > 0;
    EVP_MD_CTX_free(md);
    char sig_b64[96];
    if (!ok || !_b64(sig, sig_len, sig_b64, sizeof sig_b64)) {
        LOG_ERROR("master: could not sign a call");
        return 0;
    }
    json_object* env = json_object_new_object();
    json_object_object_add(env, "key", json_object_new_string(master.key_b64));
    json_object_object_add(env, "body", json_object_new_string(text));
    json_object_object_add(env, "sig", json_object_new_string(sig_b64));
    const char* envelope = json_object_to_json_string_ext(env, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    char        url[320];
    snprintf(url, sizeof url, "%s%s", master.url, path);
    const char*   headers[] = {"Content-Type: application/json"};
    net_request_t req       = {lane, "POST", url, headers, 1, envelope, strlen(envelope), timeout_s, family};
    uint32_t      id        = net_http(&req);
    json_object_put(env);
    return id;
}

/* ---- announce ---- */

static const char* _mode_name(server_t* server)
{
    const gamemode_ops_t* ops = gamemode_ops_of((uint8_t) server->protocol.current_gamemode);
    return ops != NULL ? ops->name : server->gamemode_name;
}

/* what the list shows: a change of it is announced at once */
static void _state(server_t* server, char* out, size_t size)
{
    snprintf(out, size, "%s|%s|%s|%d|%d", server->server_name, server->map_name, _mode_name(server),
             (int) server->protocol.num_players, (int) server->protocol.max_players);
}

static void _announce(server_t* server, int f)
{
    json_object* b = json_object_new_object();
    json_object_object_add(b, "type", json_object_new_string("announce"));
    json_object_object_add(b, "name", json_object_new_string(server->server_name));
    json_object_object_add(b, "port", json_object_new_int(server->port));
    json_object_object_add(b, "map", json_object_new_string(server->map_name));
    json_object_object_add(b, "mode", json_object_new_string(_mode_name(server)));
    /* the profile's protocol: the master's row says it as game_version and
       gives a 0.75 server the classic aos://<word>:port identifier, so the
       client's merge replaces the public 0.75 list's row of the same
       server with ours (docs/plan-075-server.md 8.6) */
    json_object_object_add(b, "protocol", json_object_new_string(profile_is_075() ? "0.75" : "1.0"));
    /* bots count, as on the 0.75 list and in HELLOLAN; info says how many.
       A 0.75 server shows the classic limit of 32, as on the public list */
    int players = server->protocol.num_players, max_players = server->protocol.max_players;
    if (profile_is_075() && max_players > MASTER_075_MAX_PLAYERS) {
        max_players = MASTER_075_MAX_PLAYERS;
    }
    if (players > max_players) {
        players = max_players;
    }
    json_object_object_add(b, "players_current", json_object_new_int(players));
    json_object_object_add(b, "players_max", json_object_new_int(max_players));
    if (master.country[0]) {
        json_object_object_add(b, "country", json_object_new_string(master.country));
    }
    json_object_object_add(b, "webrtc_port", json_object_new_int(webrtc_port()));
    if (webrtc_fingerprint()[0]) {
        json_object_object_add(b, "dtls_fingerprint", json_object_new_string(webrtc_fingerprint()));
    }
    /* the STUN and TURN servers this server uses, for the browser's
       RTCPeerConnection (a TURN relay only helps when both sides use it) */
    size_t              n_ice = 0;
    const webrtc_ice_t* ice   = webrtc_ice_servers(&n_ice);
    if (n_ice > 0) {
        json_object* list = json_object_new_array();
        for (size_t i = 0; i < n_ice; i++) {
            json_object* e = json_object_new_object();
            json_object_object_add(e, "urls", json_object_new_string(ice[i].url));
            if (ice[i].username[0]) {
                json_object_object_add(e, "username", json_object_new_string(ice[i].username));
            }
            if (ice[i].credential[0]) {
                json_object_object_add(e, "credential", json_object_new_string(ice[i].credential));
            }
            json_object_array_add(list, e);
        }
        json_object_object_add(b, "ice_servers", list);
    }
    json_object_object_add(b, "info", ping_info(server));
    /* a listen server whose host ticked "friends join by code"
       (--join-code): the master maps the code to this server and every
       player that asks for it is relayed here (its row off the public
       list unless --public asked). A 0.75 profile server joins the
       ENet way and never uncovers the code */
    if (server->join_code[0] && !profile_is_075()) {
        json_object_object_add(b, "join_code", json_object_new_string(server->join_code));
        if (server->listen_private) {
            json_object_object_add(b, "unlisted", json_object_new_boolean(1));
        }
        evt_log(NULL, "master", "action=announce join_code=%s unlisted=%d", server->join_code,
                (int) server->listen_private);
    }
    master.family[f].call = _post(NET_LANE_MASTER, "/v1/announce", b, f == 0 ? 4 : 6, 10);
    master.family[f].sent = get_real_nanos();
    json_object_put(b);
}

/* the error text of an answer, for the EVT line */
static void _why(const plugin_net_result_t* r, char* out, size_t size)
{
    if (r->status == 0) {
        snprintf(out, size, "%s", r->error);
        return;
    }
    json_object* j   = r->body != NULL ? json_tokener_parse(r->body) : NULL;
    json_object* err = NULL;
    if (j != NULL && json_object_object_get_ex(j, "error", &err)) {
        snprintf(out, size, "%ld %s", r->status, json_object_get_string(err));
    } else {
        snprintf(out, size, "%ld", r->status);
    }
    json_object_put(j);
    for (char* c = out; *c; c++) {
        if (*c == '"') {
            *c = '\'';
        }
    }
}

/* the key the master signs connect tokens with, in every announce's
   answer (it may change when the master restarts) */
static void _take_token_key(json_object* j)
{
    json_object*  key = NULL;
    unsigned char raw[48];
    const char*   b64 = NULL;
    if (j == NULL || !json_object_object_get_ex(j, "token_key", &key) ||
        (b64 = json_object_get_string(key)) == NULL || strlen(b64) != 44 ||
        EVP_DecodeBlock(raw, (const unsigned char*) b64, 44) != 33)
    {
        return; /* an older master: no tokens asked for */
    }
    if (!master.token_key_ok || memcmp(master.token_key, raw, 32) != 0) {
        LOG_INFO("master: connect tokens %s", master.tokens ? "required from players on the internet" : "off");
        evt_log(NULL, "master", "action=token_key tokens=%d", master.tokens);
    }
    memcpy(master.token_key, raw, 32);
    master.token_key_ok = 1;
    master.token_key_at = get_real_nanos();
}

static void _on_announced(int f, const plugin_net_result_t* r)
{
    master.family[f].call = 0;
    int fam               = f == 0 ? 4 : 6;
    if (r->status == 200) {
        json_object* j   = r->body != NULL ? json_tokener_parse(r->body) : NULL;
        json_object* identifier = NULL, *code = NULL;
        if (!master.family[f].listed) {
            json_object_object_get_ex(j, "identifier", &identifier);
            LOG_INFO("master: listed over IPv%d as %s", fam, identifier ? json_object_get_string(identifier) : "?");
        }
        evt_log(NULL, "master", "action=listed family=%d identifier=%s", fam,
                identifier ? json_object_get_string(identifier) : "-");
        /* the player's code, exactly as a friend reads it (`XXX-XXX`,
             the listenserver log line the host's client looks for) */
        if (json_object_object_get_ex(j, "join_code", &code) && json_object_get_string(code) != NULL) {
            const char* c = json_object_get_string(code);
            if (strlen(c) == 6) {
                LOG_STATUS("Join code: %c%c%c-%c%c%c", c[0], c[1], c[2], c[3], c[4], c[5]);
                evt_log(NULL, "master", "action=join_code code=%s", c);
            }
        }
        _take_token_key(j);
        json_object_put(j);
        master.family[f].listed        = 1;
        master.family[f].last_error[0] = '\0';
        return;
    }
    char why[96];
    _why(r, why, sizeof why);
    master.family[f].listed = 0;
    /* a host without IPv6 fails every 30 s: said once, until it changes */
    if (strcmp(why, master.family[f].last_error) != 0) {
        LOG_WARNING("master: announce over IPv%d failed: %s", fam, why);
        snprintf(master.family[f].last_error, sizeof master.family[f].last_error, "%s", why);
    }
    evt_log(NULL, "master", "action=error call=announce family=%d why=\"%s\"", fam, why);
}

/* ---- offers ---- */

static void _on_poll(const plugin_net_result_t* r)
{
    master.poll_call = 0;
    if (r->status != 200) {
        char why[96];
        _why(r, why, sizeof why);
        evt_log(NULL, "master", "action=error call=poll family=%d why=\"%s\"", master.poll_family, why);
        master.poll_after = get_real_nanos() + MASTER_RETRY_NS;
        return;
    }
    json_object* j      = r->body != NULL ? json_tokener_parse(r->body) : NULL;
    json_object* offers = NULL;
    if (j != NULL && json_object_object_get_ex(j, "offers", &offers) && json_object_is_type(offers, json_type_array))
    {
        for (size_t k = 0; k < json_object_array_length(offers); k++) {
            json_object *o = json_object_array_get_idx(offers, k), *id = NULL, *sdp = NULL, *ip = NULL;
            if (json_object_object_get_ex(o, "id", &id) && json_object_object_get_ex(o, "sdp", &sdp)) {
                json_object_object_get_ex(o, "ip", &ip);
                webrtc_offer(master.server, json_object_get_string(id), json_object_get_string(sdp),
                             ip != NULL ? json_object_get_string(ip) : "");
            }
        }
    }
    json_object_put(j);
}

static void _offer_call(const char* offer_id, const char* type, const char* key, const char* value)
{
    if (!master.on) {
        return;
    }
    json_object* b = json_object_new_object();
    json_object_object_add(b, "type", json_object_new_string(type));
    json_object_object_add(b, "offer", json_object_new_string(offer_id));
    json_object_object_add(b, key, json_object_new_string(value));
    /* over the family the offer came by: the master knows this server by it */
    if (_post(NET_LANE_MASTER_ANSWER, "/v1/answer", b, master.poll_family, 8) == 0) {
        evt_log(NULL, "master", "action=error call=%s why=queue_full", type);
    }
    json_object_put(b);
}

void master_answer(const char* offer_id, const char* sdp)
{
    _offer_call(offer_id, "answer", "sdp", sdp);
}

void master_reject(const char* offer_id, const char* reason)
{
    _offer_call(offer_id, "reject", "reason", reason);
}

/* ---- lifecycle ---- */

void master_start(server_t* server)
{
    master.server = server;
    data_path(master.key_file, sizeof master.key_file, MASTER_KEY_FILE);
    int  enabled  = 1;
    master.tokens = 1;
    FILE* fp      = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "master") : NULL;
        if (t != NULL) {
            toml_datum_t d = toml_bool_in(t, "enabled");
            if (d.ok) {
                enabled = d.u.b;
            }
            d = toml_string_in(t, "url");
            if (d.ok) {
                snprintf(master.url, sizeof master.url, "%s", d.u.s);
                free(d.u.s);
            }
            d = toml_string_in(t, "key_file");
            if (d.ok) {
                /* relative to the data folder (--data), as the default is:
                   from the root, two profiles or the macOS app shared one key */
                if (d.u.s[0] == '/') {
                    snprintf(master.key_file, sizeof master.key_file, "%s", d.u.s);
                } else {
                    data_path(master.key_file, sizeof master.key_file, d.u.s);
                }
                free(d.u.s);
            }
            d = toml_bool_in(t, "connect_tokens");
            if (d.ok) {
                master.tokens = d.u.b;
            }
            d = toml_string_in(t, "country");
            if (d.ok) {
                snprintf(master.country, sizeof master.country, "%s", d.u.s);
                free(d.u.s);
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_MASTER_URL");
    if (env != NULL && *env != '\0') {
        snprintf(master.url, sizeof master.url, "%s", env);
        enabled = 1;
    }
    env = getenv("SP_MASTER_KEY");
    if (env != NULL && *env != '\0') {
        snprintf(master.key_file, sizeof master.key_file, "%s", env);
    }
    env = getenv("SP_CONNECT_TOKENS");
    if (env != NULL && *env != '\0') {
        master.tokens = !strcmp(env, "all") ? 2 : atoi(env) != 0;
    }
    env = getenv("SP_MASTER");
    if (env != NULL && !strcmp(env, "0")) {
        enabled = 0;
    }
    /* a listen server (a player's own match with its bots, started from
       the menu) stays off the public list unless its host asked for it
       (--public): every singleplayer match was listed, and anyone could
       join it. One with a join code still announces: the code, not the
       list, is where its players find it */
    if (server->join_code[0] && profile_is_075()) {
        LOG_WARNING("master: the join code is the 1.0 profile's (a 0.75 profile join needs a forwarded port)");
        return;
    }
    if (server->listen_private && !server->join_code[0]) {
        LOG_INFO("master: not listed (a listen server; --public lists it)");
        return;
    }
    size_t n = strlen(master.url);
    while (n > 0 && master.url[n - 1] == '/') {
        master.url[--n] = '\0';
    }
    if (!enabled || n == 0) {
        LOG_INFO("master: not listed (%s)", enabled ? "no [master] url" : "[master] enabled = false");
        return;
    }
    if (strncmp(master.url, "https://", 8) != 0 && strncmp(master.url, "http://", 7) != 0) {
        LOG_WARNING("master: url %s is not http(s)://: not listed", master.url);
        return;
    }
    /* an address in the url can be reached over its own family only: an
       announce "over IPv6" to 127.0.0.1 would list the IPv4 address twice */
    char          host[256];
    unsigned char a4[4];
    master.families = 3;
    if (strstr(master.url, "://[") != NULL) {
        master.families = 2;
    } else if (plugin_net_host(master.url, host, sizeof host) && inet_pton(AF_INET, host, a4) == 1) {
        master.families = 1;
    }
    master.key = _load_or_make_key();
    unsigned char raw[32];
    size_t        raw_len = sizeof raw;
    if (master.key == NULL || EVP_PKEY_get_raw_public_key(master.key, raw, &raw_len) <= 0 ||
        !_b64(raw, raw_len, master.key_b64, sizeof master.key_b64))
    {
        LOG_WARNING("master: no server key: not listed");
        return;
    }
    master.on = 1;
    LOG_STATUS("Announcing to the master server %s (key %s)", master.url, master.key_b64);
}

void master_tick(server_t* server)
{
    if (!master.on) {
        return;
    }
    plugin_net_result_t r;
    while (net_done(NET_LANE_MASTER, &r)) {
        for (int f = 0; f < 2; f++) {
            if (r.id == master.family[f].call) {
                _on_announced(f, &r);
            }
        }
        free(r.body);
    }
    while (net_done(NET_LANE_MASTER_POLL, &r)) {
        if (r.id == master.poll_call) {
            _on_poll(&r);
        }
        free(r.body);
    }
    while (net_done(NET_LANE_MASTER_ANSWER, &r)) {
        if (r.status != 200) {
            char why[96];
            _why(&r, why, sizeof why);
            evt_log(NULL, "master", "action=error call=answer why=\"%s\"", why);
        }
        free(r.body);
    }
    if (server->map_name[0] == '\0') {
        return; /* between maps */
    }

    char state[sizeof master.family[0].state];
    _state(server, state, sizeof state);
    uint64_t now = get_real_nanos();
    for (int f = 0; f < 2; f++) {
        if (master.family[f].call != 0 || !(master.families & (1 << f))) {
            continue;
        }
        uint64_t since   = now - master.family[f].sent;
        int      changed = strcmp(state, master.family[f].state) != 0;
        if (master.family[f].sent == 0 || since >= MASTER_PERIOD_NS || (changed && since >= MASTER_CHANGE_NS)) {
            _announce(server, f);
            snprintf(master.family[f].state, sizeof master.family[f].state, "%s", state);
        }
    }

    /* one poll waiting, over a family the master lists */
    if (webrtc_port() != 0 && master.poll_call == 0 && now >= master.poll_after &&
        (master.family[0].listed || master.family[1].listed))
    {
        master.poll_family = master.family[0].listed ? 4 : 6;
        json_object* b     = json_object_new_object();
        json_object_object_add(b, "type", json_object_new_string("poll"));
        master.poll_call = _post(NET_LANE_MASTER_POLL, "/v1/poll", b, master.poll_family, MASTER_POLL_TIMEOUT_S);
        json_object_put(b);
        if (master.poll_call == 0) {
            master.poll_after = now + MASTER_RETRY_NS;
        }
    }
}

void master_stop(void)
{
    if (!master.on) {
        return;
    }
    uint32_t calls[2] = {0, 0};
    for (int f = 0; f < 2; f++) {
        if (master.family[f].listed) {
            json_object* b = json_object_new_object();
            json_object_object_add(b, "type", json_object_new_string("leave"));
            calls[f] = _post(NET_LANE_MASTER, "/v1/announce", b, f == 0 ? 4 : 6, 3);
            json_object_put(b);
        }
    }
    /* the leave must go before the process does: wait for it, 3 s at most
       (the announces still in flight come back first on the same lane) */
    uint64_t until = get_real_nanos() + 3ULL * NANO_IN_SECOND;
    while ((calls[0] || calls[1]) && get_real_nanos() < until) {
        plugin_net_result_t r;
        while (net_done(NET_LANE_MASTER, &r)) {
            for (int f = 0; f < 2; f++) {
                if (calls[f] != 0 && r.id == calls[f]) {
                    calls[f] = 0;
                    if (r.status == 200) {
                        evt_log(NULL, "master", "action=left family=%d", f == 0 ? 4 : 6);
                    } else {
                        char why[96];
                        _why(&r, why, sizeof why);
                        evt_log(NULL, "master", "action=error call=leave family=%d why=\"%s\"", f == 0 ? 4 : 6,
                                why);
                    }
                }
            }
            free(r.body);
        }
        struct timespec ms = {0, 20 * 1000000L};
        nanosleep(&ms, NULL);
    }
    EVP_PKEY_free(master.key);
    master.key = NULL;
    master.on  = 0;
}

/* ---- connect tokens (the master's tokens.go) ----
 *
 * A 1.0 player from the internet joins only with a token from our master,
 * sent as its ClientCertificate: certificate "SADT" | 1 | server key (32) |
 * expiry u64 LE (unix s) | nonce length u8 | nonce, signed Ed25519 by the
 * master. It names this server and the nonce of this connection's Greeting,
 * so a flood of joins has to ask the master, one per connection, under its
 * per address limit. Not asked: a local address (the LAN, the listen
 * server's own player), a WebRTC connection (its offer came through the
 * master already), and while this server has not heard the master's key in
 * MASTER_TOKEN_TRUST: a master that is down must not lock everyone out. */

#define MASTER_TOKEN_TRUST (120ULL * NANO_IN_SECOND)
#define TOKEN_HEAD         (4 + 1 + 32 + 8 + 1)

static int _address_local(const char* address)
{
    unsigned char a[16];
    if (inet_pton(AF_INET, address, a) == 1) {
        return a[0] == 10 || a[0] == 127 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
               (a[0] == 169 && a[1] == 254);
    }
    if (inet_pton(AF_INET6, address, a) == 1) {
        static const unsigned char loopback[16] = {[15] = 1};
        return !memcmp(a, loopback, 16) || (a[0] & 0xFE) == 0xFC || (a[0] == 0xFE && (a[1] & 0xC0) == 0x80);
    }
    return 0;
}

const char* master_token_needed(const conn_t* conn)
{
    if (!master.on || !master.tokens || conn == NULL) {
        return NULL;
    }
    if (!master.token_key_ok || get_real_nanos() - master.token_key_at > MASTER_TOKEN_TRUST) {
        return NULL;
    }
    if (strcmp(conn->transport->name, "enet") != 0) {
        return NULL;
    }
    if (master.tokens < 2 && _address_local(conn->address)) {
        return NULL;
    }
    return "internet";
}

int master_token_check(const uint8_t* nonce,
                       size_t         nonce_len,
                       const uint8_t* cert,
                       size_t         cert_len,
                       const uint8_t* sig,
                       size_t         sig_len,
                       const char**   why)
{
    unsigned char own[32];
    size_t        own_len = sizeof own;
    if (cert == NULL || cert_len < TOKEN_HEAD || memcmp(cert, "SADT", 4) != 0 || cert[4] != 1) {
        *why = "format";
        return 0;
    }
    if (EVP_PKEY_get_raw_public_key(master.key, own, &own_len) <= 0 || memcmp(cert + 5, own, 32) != 0) {
        *why = "server";
        return 0;
    }
    uint64_t expiry = 0;
    for (int i = 7; i >= 0; i--) {
        expiry = expiry << 8 | cert[37 + i];
    }
    if (expiry < _unix_ms() / 1000ULL) {
        *why = "expired";
        return 0;
    }
    if (cert[45] != nonce_len || cert_len != TOKEN_HEAD + nonce_len || memcmp(cert + TOKEN_HEAD, nonce, nonce_len) != 0) {
        *why = "nonce";
        return 0;
    }
    EVP_PKEY*   pub = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, master.token_key, 32);
    EVP_MD_CTX* md  = EVP_MD_CTX_new();
    int         ok  = pub != NULL && md != NULL && sig != NULL && sig_len == 64 &&
             EVP_DigestVerifyInit(md, NULL, NULL, NULL, pub) > 0 &&
             EVP_DigestVerify(md, sig, sig_len, cert, cert_len) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pub);
    if (!ok) {
        *why = "signature";
        return 0;
    }
    return 1;
}
