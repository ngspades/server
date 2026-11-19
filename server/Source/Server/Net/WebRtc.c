// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Net/Master.h>
#include <Server/Net/RtcPeer.h>
#include <Server/Net/WebRtc.h>
#include <Server/Player.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/DataDir.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <tomlc99/toml.h>

/* the browser waits 10 s for the answer through the master: answer within
   8 or say no */
#define WEBRTC_ANSWER_NS (8ULL * NANO_IN_SECOND)
/* from the answer to "connect": the DTLS and SCTP handshakes and the two
   channels opening take a second or two on a slow line */
#define WEBRTC_CONNECT_NS (20ULL * NANO_IN_SECOND)
/* a goodbye waits for the queued packets (the Kick's text) this long */
#define WEBRTC_FLUSH_NS (2ULL * NANO_IN_SECOND)
/* browsers take messages up to 64-256 KB; the contract promises 16 */
#define WEBRTC_MAX_MESSAGE 16384
/* offers being answered or peers not yet in the game, at most */
#define WEBRTC_MAX_PENDING 32

#define WEBRTC_CERT_NAME "webrtc.pem" /* the certificate and its key, in the data folder */
#define WEBRTC_CERT_FILE _cert_file()

static const char* _cert_file(void)
{
    static char path[1100];
    return data_path(path, sizeof path, WEBRTC_CERT_NAME);
}

typedef struct webrtc_peer
{
    conn_t              conn; /* conn.handle points back here */
    uint32_t            id;   /* RtcPeer.cpp's */
    char                offer_id[64];
    uint64_t            created;  /* real nanos */
    uint64_t            close_at; /* closing: real nanos, when it began */
    uint8_t             answered;
    uint8_t             connected; /* "connect" came */
    uint8_t             open_mask; /* channels open, 1 << channel */
    uint8_t             closing;
    uint8_t             close_event; /* closing: tell the game when closed */
    uint8_t             dead;        /* closed, freed once no player points at it */
    uint8_t             warned_size;
    struct webrtc_peer* next;
} webrtc_peer_t;

static webrtc_peer_t* rtc_peers;
static uint16_t       rtc_port;
static char           rtc_fingerprint[160];

uint16_t webrtc_port(void)
{
    return rtc_port;
}

const char* webrtc_fingerprint(void)
{
    return rtc_fingerprint;
}

/* ---- the conn_t of a peer ---- */

static int _rtc_send(conn_t* conn, uint8_t channel, const uint8_t* data, size_t len, int reliable)
{
    (void) reliable; /* the channel says it: 0 reliable, 1 unreliable */
    webrtc_peer_t* p = (webrtc_peer_t*) conn->handle;
    if (p->closing || p->dead) {
        return -1;
    }
    if (len > WEBRTC_MAX_MESSAGE && !p->warned_size) {
        /* sent anyway (a browser takes it), but the contract says 16 KB:
           the sender must be cut */
        p->warned_size = 1;
        evt_log(NULL, "webrtc", "action=too_big peer=%u len=%zu type=%u", p->id, len, len ? data[0] : 0);
    }
    return rtcw_send(p->id, channel, data, len, 0);
}

static void _rtc_disconnect(conn_t* conn, uint32_t reason, conn_close_t how)
{
    webrtc_peer_t* p = (webrtc_peer_t*) conn->handle;
    if (p->closing || p->dead) {
        return;
    }
    /* the reason ENet would carry in its disconnect data, as text: a
       binary message is always a game packet */
    char text[32];
    int  n = snprintf(text, sizeof text, "disconnect %u", (unsigned) reason);
    rtcw_send(p->id, 0, text, (size_t) n, 1);
    p->closing  = 1;
    p->close_at = get_real_nanos();
    /* ENet's disconnect_now brings no event back: neither does this */
    p->close_event = how != CONN_CLOSE_NOW;
    evt_log(NULL, "webrtc", "action=closing peer=%u reason=%u how=%s", p->id, (unsigned) reason,
            how == CONN_CLOSE_NOW ? "now" : (how == CONN_CLOSE_LATER ? "later" : "close"));
}

static uint32_t _rtc_rtt(const conn_t* conn)
{
    const webrtc_peer_t* p = (const webrtc_peer_t*) conn->handle;
    return p->dead ? 0 : rtcw_rtt(p->id);
}

/* what libdatachannel's SCTP still holds for the peer: the same figure its
   flush check reads below, so the map transfer paces itself the same way
   over a DataChannel as over ENet */
static size_t _rtc_pending(const conn_t* conn)
{
    const webrtc_peer_t* p = (const webrtc_peer_t*) conn->handle;
    return p->dead ? 0 : rtcw_buffered(p->id);
}

static const conn_transport_t webrtc_transport = {"webrtc", _rtc_send, _rtc_disconnect, _rtc_rtt, _rtc_pending};

int webrtc_count_from(uint32_t key, const conn_t* except)
{
    int n = 0;
    for (webrtc_peer_t* p = rtc_peers; p != NULL; p = p->next) {
        if (&p->conn != except && p->connected && !p->closing && !p->dead && p->conn.key == key) {
            n++;
        }
    }
    return n;
}

/* ---- the DTLS certificate: one for every peer, kept in the data folder,
   so the fingerprint the master lists stays the same across restarts ---- */

/* the certificate and its key, from the one file holding both; NULL when
   missing, broken or out of date (good for a week more at least: browsers
   do not check the dates, a later DTLS library might) */
static int _load_cert(X509** cert_out, EVP_PKEY** key_out)
{
    FILE* f = fopen(WEBRTC_CERT_FILE, "r");
    if (f == NULL) {
        return 0;
    }
    X509*     cert = PEM_read_X509(f, NULL, NULL, NULL);
    EVP_PKEY* key  = cert != NULL ? PEM_read_PrivateKey(f, NULL, NULL, NULL) : NULL;
    fclose(f);
    time_t week = time(NULL) + 7 * 24 * 3600;
    if (cert == NULL || key == NULL || X509_check_private_key(cert, key) != 1 ||
        X509_cmp_time(X509_get0_notAfter(cert), &week) <= 0)
    {
        X509_free(cert);
        EVP_PKEY_free(key);
        return 0;
    }
    *cert_out = cert;
    *key_out  = key;
    return 1;
}

/* one file, written aside and renamed: servers starting together from one
   data folder (the benchmarks) never read a certificate with another's key */
static int _make_cert(void)
{
    EVP_PKEY* key  = EVP_EC_gen("P-256");
    X509*     cert = X509_new();
    int       ok   = key != NULL && cert != NULL;
    if (ok) {
        unsigned char serial[8];
        FILE*         r = fopen("/dev/urandom", "rb");
        if (r == NULL || fread(serial, 1, sizeof serial, r) != sizeof serial) {
            for (size_t i = 0; i < sizeof serial; i++) {
                serial[i] = (unsigned char) (get_real_nanos() >> (i * 4));
            }
        }
        if (r != NULL) {
            fclose(r);
        }
        serial[0] &= 0x7F;
        BIGNUM* bn = BN_bin2bn(serial, sizeof serial, NULL);
        BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert));
        BN_free(bn);
        X509_set_version(cert, 2);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 365L * 24 * 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*) GAME_SERVER_BINARY, -1, -1, 0);
        X509_set_issuer_name(cert, name);
        ok = X509_sign(cert, key, EVP_sha256()) > 0;
    }
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", WEBRTC_CERT_FILE, (long) getpid());
    FILE* f = ok ? fopen(tmp, "w") : NULL;
    ok      = f != NULL && PEM_write_X509(f, cert) && PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL);
    if (f != NULL) {
        fclose(f);
    }
    ok = ok && rename(tmp, WEBRTC_CERT_FILE) == 0;
    if (!ok) {
        remove(tmp);
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

/* a certificate or a key as PEM text, malloc'd */
static char* _pem(X509* cert, EVP_PKEY* key)
{
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == NULL) {
        return NULL;
    }
    int ok = cert != NULL ? PEM_write_bio_X509(bio, cert) : PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL);
    char* data = NULL;
    long  len  = ok ? BIO_get_mem_data(bio, &data) : 0;
    char* out  = len > 0 ? malloc((size_t) len + 1) : NULL;
    if (out != NULL) {
        memcpy(out, data, (size_t) len);
        out[len] = '\0';
    }
    BIO_free(bio);
    return out;
}

static void _fingerprint(X509* cert)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n = 0;
    rtc_fingerprint[0] = '\0';
    if (!X509_digest(cert, EVP_sha256(), md, &n)) {
        return;
    }
    size_t at = (size_t) snprintf(rtc_fingerprint, sizeof rtc_fingerprint, "sha-256 ");
    for (unsigned int i = 0; i < n && at + 4 < sizeof rtc_fingerprint; i++) {
        at += (size_t) snprintf(rtc_fingerprint + at, sizeof rtc_fingerprint - at, i ? ":%02X" : "%02X", md[i]);
    }
}

static void _rtc_log(int warning, const char* message)
{
    if (warning) {
        LOG_WARNING("webrtc: %s", message);
    } else {
        LOG_INFO("webrtc: %s", message);
    }
}

/* ---- the ICE servers (docs/plan-075-server.md 10.3): STUN alone leaves
   the players behind a symmetric NAT (many mobile operators) out, a TURN
   relay lets them in ---- */

static webrtc_ice_t rtc_ice[WEBRTC_MAX_ICE];
static size_t       rtc_ice_count;

const webrtc_ice_t* webrtc_ice_servers(size_t* n)
{
    *n = rtc_port != 0 ? rtc_ice_count : 0;
    return rtc_ice;
}

/* one entry of a list; a URL without its scheme gets the key's ("host:3478"
   under turn is "turn:host:3478"), credentials only for TURN */
static void _ice_add(const char* scheme, const char* item, size_t len, const char* user, const char* cred)
{
    while (len > 0 && (*item == ' ' || *item == '\t')) {
        item++;
        len--;
    }
    while (len > 0 && (item[len - 1] == ' ' || item[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) {
        return;
    }
    if (rtc_ice_count >= WEBRTC_MAX_ICE) {
        LOG_WARNING("webrtc: more than %d ICE servers, %.*s left out", WEBRTC_MAX_ICE, (int) len, item);
        return;
    }
    webrtc_ice_t* e        = &rtc_ice[rtc_ice_count];
    int           scheme_d = !strncmp(item, "stun:", 5) || !strncmp(item, "stuns:", 6) ||
                   !strncmp(item, "turn:", 5) || !strncmp(item, "turns:", 6);
    int n = scheme_d ? snprintf(e->url, sizeof e->url, "%.*s", (int) len, item)
                     : snprintf(e->url, sizeof e->url, "%s:%.*s", scheme, (int) len, item);
    if (n < 0 || (size_t) n >= sizeof e->url) {
        LOG_WARNING("webrtc: ICE server %.*s is too long, left out", (int) len, item);
        return;
    }
    e->username[0] = e->credential[0] = '\0';
    if (!strncmp(e->url, "turn", 4)) {
        snprintf(e->username, sizeof e->username, "%s", user);
        snprintf(e->credential, sizeof e->credential, "%s", cred);
    }
    rtc_ice_count++;
}

static void _ice_add_list(const char* scheme, const char* csv, const char* user, const char* cred)
{
    while (csv != NULL && *csv != '\0') {
        const char* comma = strchr(csv, ',');
        size_t      len   = comma != NULL ? (size_t) (comma - csv) : strlen(csv);
        _ice_add(scheme, csv, len, user, cred);
        csv = comma != NULL ? comma + 1 : NULL;
    }
}

/* [webrtc] <key>: a string (a comma separated list) or an array of them,
   joined into out as one list; 1 when the key was there */
static int _ice_key(toml_table_t* t, const char* key, char* out, size_t size)
{
    toml_datum_t d = toml_string_in(t, key);
    if (d.ok) {
        snprintf(out, size, "%s", d.u.s);
        free(d.u.s);
        return 1;
    }
    toml_array_t* a = toml_array_in(t, key);
    if (a == NULL) {
        return 0;
    }
    out[0]    = '\0';
    size_t at = 0;
    for (int i = 0; i < toml_array_nelem(a); i++) {
        toml_datum_t s = toml_string_at(a, i);
        if (s.ok) {
            int n = snprintf(out + at, size - at, "%s%s", at ? "," : "", s.u.s);
            if (n > 0 && at + (size_t) n < size) {
                at += (size_t) n;
            }
            free(s.u.s);
        }
    }
    return 1;
}

static void _ice_env(const char* name, char* out, size_t size)
{
    const char* env = getenv(name);
    if (env != NULL) {
        snprintf(out, size, "%s", env);
    }
}

void webrtc_start(server_t* server)
{
    long port    = (long) server->port + 1;
    int  enabled = 0;
    char stun[1024], turn[1024] = "", turns[1024] = "", turn_user[128] = "", turn_cred[128] = "";
    snprintf(stun, sizeof stun, "stun:stun.l.google.com:19302");
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "webrtc") : NULL;
        if (t != NULL) {
            toml_datum_t d = toml_int_in(t, "port");
            if (d.ok && d.u.i > 0) {
                port = (long) d.u.i;
            }
            d = toml_bool_in(t, "enabled");
            if (d.ok) {
                enabled = d.u.b;
            }
            _ice_key(t, "stun", stun, sizeof stun);
            _ice_key(t, "turn", turn, sizeof turn);
            _ice_key(t, "turns", turns, sizeof turns);
            d = toml_string_in(t, "turn_username");
            if (d.ok) {
                snprintf(turn_user, sizeof turn_user, "%s", d.u.s);
                free(d.u.s);
            }
            d = toml_string_in(t, "turn_credential");
            if (d.ok) {
                snprintf(turn_cred, sizeof turn_cred, "%s", d.u.s);
                free(d.u.s);
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    /* WebRTC is the players' own matches' (a join code: the friend's
       client, or a browser, comes through the master's signalling); a
       dedicated server opens it too when its config says so */
    const char* on = getenv("SP_WEBRTC_ENABLED");
    if (on != NULL && *on != '\0') {
        enabled = atoi(on) != 0;
    }
    if (server->join_code[0]) {
        enabled = 1;
    }
    if (!enabled) {
        LOG_INFO("WebRTC: off (a join code turns it on; [webrtc] enabled for a dedicated server)");
        return;
    }
    const char* env = getenv("SP_WEBRTC_PORT");
    if (env != NULL && *env != '\0') {
        port = strtol(env, NULL, 10);
    }
    _ice_env("SP_WEBRTC_STUN", stun, sizeof stun);
    _ice_env("SP_WEBRTC_TURN", turn, sizeof turn);
    _ice_env("SP_WEBRTC_TURNS", turns, sizeof turns);
    _ice_env("SP_WEBRTC_TURN_USERNAME", turn_user, sizeof turn_user);
    _ice_env("SP_WEBRTC_TURN_CREDENTIAL", turn_cred, sizeof turn_cred);
    rtc_ice_count = 0;
    _ice_add_list("stun", stun, "", "");
    _ice_add_list("turn", turn, turn_user, turn_cred);
    _ice_add_list("turns", turns, turn_user, turn_cred);
    if ((turn[0] || turns[0]) && !turn_user[0]) {
        LOG_WARNING("webrtc: TURN without turn_username / turn_credential: most relays refuse it");
    }
    rtcw_ice_t ice[WEBRTC_MAX_ICE];
    char       ice_text[600] = "";
    size_t     ice_at        = 0;
    for (size_t i = 0; i < rtc_ice_count; i++) {
        ice[i].url        = rtc_ice[i].url;
        ice[i].username   = rtc_ice[i].username;
        ice[i].credential = rtc_ice[i].credential;
        /* never the credential in a log */
        int n = snprintf(ice_text + ice_at, sizeof ice_text - ice_at, "%s%s", i ? ", " : "", rtc_ice[i].url);
        if (n > 0 && ice_at + (size_t) n < sizeof ice_text) {
            ice_at += (size_t) n;
        }
    }
    if (port <= 0 || port > 65535) {
        LOG_WARNING("webrtc: port %ld is not a port: no browser can join", port);
        return;
    }

    X509*     cert = NULL;
    EVP_PKEY* key  = NULL;
    if (!_load_cert(&cert, &key) && _make_cert()) {
        LOG_INFO("webrtc: made a DTLS certificate (%s)", WEBRTC_CERT_FILE);
    }
    if (cert == NULL) {
        _load_cert(&cert, &key); /* this one's, or a server's that started with it */
    }
    char* cert_pem = cert != NULL ? _pem(cert, NULL) : NULL;
    char* key_pem  = key != NULL ? _pem(NULL, key) : NULL;
    char  err[256] = "";
    if (cert_pem == NULL || key_pem == NULL) {
        snprintf(err, sizeof err, "no DTLS certificate (%s)", WEBRTC_CERT_FILE);
    } else {
        _fingerprint(cert);
        if (rtcw_init((uint16_t) port, ice, rtc_ice_count, cert_pem, key_pem, _rtc_log, err, sizeof err)) {
            rtc_port = (uint16_t) port;
        }
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    free(cert_pem);
    free(key_pem);
    if (rtc_port == 0) {
        LOG_WARNING("webrtc: not started (%s): no browser can join", err);
        return;
    }
    LOG_STATUS("WebRTC (browsers) on UDP port %u, ICE servers: %s", (unsigned) rtc_port,
               rtc_ice_count ? ice_text : "none");
    evt_log(NULL, "webrtc", "action=start port=%u ice=%zu ice_servers=\"%s\"", (unsigned) rtc_port, rtc_ice_count,
            ice_text);
}

/* ---- offers ---- */

static int _pending(void)
{
    int n = 0;
    for (webrtc_peer_t* p = rtc_peers; p != NULL; p = p->next) {
        n += !p->connected && !p->dead;
    }
    return n;
}

/* the browser's address as the ban check reads a connection's */
static int _offer_banned(const char* ip, int v4, const uint8_t ip4[4])
{
    conn_t probe;
    memset(&probe, 0, sizeof probe);
    probe.family = v4 ? 4 : 6;
    memcpy(probe.ip4, ip4, 4);
    snprintf(probe.address, sizeof probe.address, "%s", ip);
    return player_banned(&probe);
}

void webrtc_offer(server_t* server, const char* offer_id, const char* sdp, const char* ip)
{
    const char* why = NULL;
    uint32_t    key = conn_key_of(ip);
    uint8_t     ip4[4] = {0, 0, 0, 0};
    int         v4     = sscanf(ip, "%hhu.%hhu.%hhu.%hhu", &ip4[0], &ip4[1], &ip4[2], &ip4[3]) == 4;
    if (rtc_port == 0) {
        why = "this server takes no browsers now";
    } else if (server->protocol.num_players >= server->protocol.max_players) {
        why = "server full";
    } else if (_offer_banned(ip, v4, ip4)) {
        why = "banned";
    } else if (_pending() >= WEBRTC_MAX_PENDING) {
        why = "too many joining, try again";
    }
    char err[256] = "";
    uint32_t id   = why == NULL ? rtcw_peer_create(sdp, err, sizeof err) : 0;
    if (why == NULL && id == 0) {
        why = "bad offer";
        LOG_WARNING("webrtc: offer %s refused: %s", offer_id, err);
    }
    if (why != NULL) {
        evt_log(NULL, "webrtc", "action=offer offer=%s ip=%s ok=0 why=\"%s\"", offer_id, ip, why);
        master_reject(offer_id, why);
        return;
    }
    webrtc_peer_t* p = calloc(1, sizeof *p);
    if (p == NULL) {
        rtcw_close(id);
        master_reject(offer_id, "server error");
        return;
    }
    p->id             = id;
    p->created        = get_real_nanos();
    p->conn.transport = &webrtc_transport;
    p->conn.handle    = p;
    p->conn.player_id = CONN_NO_PLAYER;
    p->conn.family    = v4 ? 4 : 6;
    memcpy(p->conn.ip4, ip4, 4);
    snprintf(p->conn.address, sizeof p->conn.address, "%s", ip);
    p->conn.key = key;
    snprintf(p->offer_id, sizeof p->offer_id, "%s", offer_id);
    p->next   = rtc_peers;
    rtc_peers = p;
    evt_log(NULL, "webrtc", "action=offer offer=%s ip=%s ok=1 peer=%u", offer_id, ip, id);
}

/* ---- events ---- */

static webrtc_peer_t* _peer(uint32_t id)
{
    for (webrtc_peer_t* p = rtc_peers; p != NULL; p = p->next) {
        if (p->id == id && !p->dead) {
            return p;
        }
    }
    return NULL;
}

/* the peer is gone; the game hears of it when it had a player */
static void _gone(server_t* server, webrtc_peer_t* p, int tell, const char* why)
{
    if (p->dead) {
        return;
    }
    rtcw_close(p->id);
    p->dead = 1;
    evt_log(NULL, "webrtc", "action=closed peer=%u player=%d why=%s", p->id,
            p->conn.player_id == CONN_NO_PLAYER ? -1 : (int) p->conn.player_id, why);
    if (tell && p->conn.player_id != CONN_NO_PLAYER) {
        server_conn_closed(server, &p->conn);
    }
    p->conn.player_id = CONN_NO_PLAYER;
}

static void _on_text(server_t* server, webrtc_peer_t* p, const char* text)
{
    unsigned long data;
    char          tail;
    /* the client's goodbye (net.js, transport_rtc.c send "disconnect <n>"
       as they leave): the slot is free now, not when the channels time out */
    if (strncmp(text, "disconnect", 10) == 0 && p->connected) {
        _gone(server, p, 1, "peer_disconnect");
        return;
    }
    if (strncmp(text, "connect ", 8) != 0 || p->connected || sscanf(text + 8, "%lu%c", &data, &tail) != 1) {
        evt_log(NULL, "webrtc", "action=text_ignored peer=%u connected=%u", p->id, p->connected);
        return;
    }
    p->connected = 1;
    char remote[96];
    rtcw_remote_address(p->id, remote, sizeof remote);
    server_conn_connect(server, &p->conn, (uint32_t) data);
    /* after: the player it became, -1 when it was refused */
    evt_log(NULL, "webrtc", "action=connected peer=%u player=%d ip=%s ice=%s data=%lu channels=%u", p->id,
            p->conn.player_id == CONN_NO_PLAYER ? -1 : (int) p->conn.player_id, p->conn.address,
            remote[0] ? remote : "-", data, p->open_mask);
}

static void _on_event(server_t* server, rtcw_event_t* ev)
{
    webrtc_peer_t* p = _peer(ev->peer);
    if (p == NULL) {
        return;
    }
    switch (ev->kind) {
        case RTCW_GATHERED:
        {
            if (p->answered) {
                break;
            }
            char* sdp = rtcw_local_sdp(p->id);
            if (sdp == NULL) {
                master_reject(p->offer_id, "server error");
                _gone(server, p, 0, "no_answer");
                break;
            }
            p->answered = 1;
            master_answer(p->offer_id, sdp);
            evt_log(NULL, "webrtc", "action=answer peer=%u offer=%s after_ms=%llu", p->id, p->offer_id,
                    (unsigned long long) ((get_real_nanos() - p->created) / NANO_IN_MILLI));
            free(sdp);
            break;
        }
        case RTCW_CHANNEL_OPEN:
            /* both its callback and the check after setting it may say so */
            if (!(p->open_mask & (1u << ev->channel))) {
                p->open_mask |= (uint8_t) (1u << ev->channel);
                evt_log(NULL, "webrtc", "action=channel peer=%u channel=%u", p->id, ev->channel);
            }
            break;
        case RTCW_MESSAGE:
            if (p->closing) {
                break;
            }
            if (ev->text) {
                _on_text(server, p, (const char*) ev->data);
            } else if (p->conn.player_id != CONN_NO_PLAYER) {
                server_conn_receive(server, &p->conn, ev->data, ev->len);
            }
            break;
        case RTCW_CLOSED:
            _gone(server, p, 1, ev->why);
            break;
    }
}

/* a goodbye's text, and the packets before it, have left: the queue is
   empty and SCTP had a moment to have them acknowledged (closing at once
   lost the "disconnect" text), or the wait is over */
static int _flushed(webrtc_peer_t* p, uint64_t now)
{
    uint64_t waited = now - p->close_at;
    return waited > WEBRTC_FLUSH_NS || (waited > 250ULL * NANO_IN_MILLI && rtcw_buffered(p->id) == 0);
}

void webrtc_poll(server_t* server)
{
    if (rtc_port == 0) {
        return;
    }
    rtcw_event_t ev;
    while (rtcw_poll(&ev)) {
        _on_event(server, &ev);
        free(ev.data);
    }
    uint64_t        now = get_real_nanos();
    webrtc_peer_t** at  = &rtc_peers;
    while (*at != NULL) {
        webrtc_peer_t* p = *at;
        if (!p->dead && !p->answered && now - p->created > WEBRTC_ANSWER_NS) {
            master_reject(p->offer_id, "the server could not answer in time");
            _gone(server, p, 0, "answer_timeout");
        } else if (!p->dead && !p->connected && !p->closing && now - p->created > WEBRTC_CONNECT_NS) {
            _gone(server, p, 0, "no_connect");
        } else if (!p->dead && p->closing && _flushed(p, now)) {
            _gone(server, p, p->close_event, "server");
        }
        /* a player kept for its grenades still points at its conn, as one
           still points at its ENet peer slot: free only once none does */
        if (p->dead && !server_conn_in_use(server, &p->conn)) {
            *at = p->next;
            free(p);
            continue;
        }
        at = &p->next;
    }
}

void webrtc_stop(void)
{
    if (rtc_port == 0) {
        return;
    }
    rtcw_shutdown();
    while (rtc_peers != NULL) {
        webrtc_peer_t* p = rtc_peers;
        rtc_peers        = p->next;
        free(p);
    }
    rtc_port = 0;
}
