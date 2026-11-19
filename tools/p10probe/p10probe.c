/*
 * p10probe: headless checks of a server's protocol 1.0 handshake and of the
 * error paths the GUI client never exercises (docs/protocol-1.0.md 2, 3, 5-7).
 *
 *   p10probe <host> <port> <scenario>
 *   scenarios: handshake ngmap mismatch idle garbage early vanilla unknown all
 *
 * handshake expects the map as VXL, which a 1.0 server sends only for a map
 * with no stored ngmap whose encoding failed: run_probe.py starts it on
 * Creative_Flat with SP_NGMAP_FAIL=1. ngmap expects the stored ngmap, twice:
 * the second join is the same stream (docs/protocol-1.0.md 6.2, 7.1).
 *
 * One line per scenario: PASS/FAIL, expected, observed. Exit 1 if any failed.
 */
#include <enet/enet.h>
#include <ngmap/ngmap.h>
#include <proto10/codec.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

/* vanilla connect data and the first packet a 0.75 server sends */
#define VANILLA_075          3u
#define VANILLA_MAP_START    18
#define VANILLA_WRONG_PROTO  3u
#define UNKNOWN_CONNECT_DATA 99u

/* the server gives a connection 5 s for InitiateConnection
   (P10_TIMEOUT_NOT_INITIATED); allow ENet some slack */
#define IDLE_MIN_S 4.0
#define IDLE_MAX_S 9.0

/* InitiateConnection.mapQuality of the next connection: 0 = VXL */
static uint8_t g_map_quality = 0;

typedef struct {
    ENetHost* host;
    ENetPeer* peer;
    int       connected;
    int       disconnected;
    uint32_t  disconnect_data;
    double    disconnect_at;
} conn_t;

typedef struct {
    uint8_t* data;
    size_t   size;
} packet_t;

typedef struct {
    const char* name;
    int         pass;
    char        expected[256];
    char        observed[1200];
} result_t;

static const char* g_host;
static uint16_t    g_port;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void set_text(char* out, size_t size, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, size, fmt, ap);
    va_end(ap);
}

static int remaining_ms(double deadline)
{
    double left = deadline - now_s();
    return left <= 0 ? 0 : (int) (left * 1000.0) + 1;
}

/* Waits for one event. Returns 1 for a received packet (copied into *out),
   -1 when the peer disconnected, 0 on timeout. */
static int conn_poll(conn_t* c, double deadline, packet_t* out)
{
    ENetEvent ev;
    while (!c->disconnected) {
        int ms = remaining_ms(deadline);
        int rc = enet_host_service(c->host, &ev, (enet_uint32) ms);
        if (rc < 0) {
            return 0;
        }
        if (rc == 0) {
            if (ms == 0) {
                return 0;
            }
            continue;
        }
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                c->connected = 1;
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: /* enet6 reports a timeout apart; the game treats it as a disconnect */
                c->disconnected    = 1;
                c->disconnect_data = ev.data;
                c->disconnect_at   = now_s();
                c->peer            = NULL;
                return -1;
            case ENET_EVENT_TYPE_RECEIVE:
                if (out != NULL) {
                    out->size = ev.packet->dataLength;
                    out->data = (uint8_t*) malloc(out->size ? out->size : 1);
                    if (out->data != NULL) {
                        memcpy(out->data, ev.packet->data, out->size);
                    }
                }
                enet_packet_destroy(ev.packet);
                if (out != NULL && out->data == NULL) {
                    return 0;
                }
                return 1;
            default:
                break;
        }
        if (out == NULL && c->connected) {
            return 0; /* conn_open only waits for the connect */
        }
    }
    return -1;
}

static int conn_open(conn_t* c, uint32_t connect_data, double timeout_s)
{
    ENetAddress addr;
    memset(c, 0, sizeof *c);
    /* the fork takes the address type first (see server/Source/Server/Master.c) */
    c->host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, NULL, 1, 1, 0, 0);
    if (c->host == NULL) {
        return 0;
    }
    /* vanilla and 1.0 both use the range coder; without it nothing decodes */
    enet_host_compress_with_range_coder(c->host);
    if (enet_address_set_host(&addr, ENET_ADDRESS_TYPE_IPV4, g_host) != 0) {
        return 0;
    }
    addr.port = g_port;
    c->peer   = enet_host_connect(c->host, &addr, 1, connect_data);
    if (c->peer == NULL) {
        return 0;
    }
    double deadline = now_s() + timeout_s;
    while (!c->connected && !c->disconnected && now_s() < deadline) {
        conn_poll(c, deadline, NULL);
    }
    return c->connected;
}

static void conn_send(conn_t* c, const uint8_t* data, size_t size)
{
    if (c->peer == NULL) {
        return;
    }
    ENetPacket* p = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(c->peer, 0, p) != 0) {
        enet_packet_destroy(p);
    }
    enet_host_flush(c->host);
}

/* a clean goodbye, so the server frees the slot before the next scenario */
static void conn_close(conn_t* c)
{
    if (c->host == NULL) {
        return;
    }
    if (c->peer != NULL && !c->disconnected) {
        enet_peer_disconnect(c->peer, 0);
        double    deadline = now_s() + 1.0;
        ENetEvent ev;
        while (now_s() < deadline && enet_host_service(c->host, &ev, 50) >= 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            } else if ((ev.type == ENET_EVENT_TYPE_DISCONNECT || ev.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT)) {
                break;
            }
        }
    }
    enet_host_destroy(c->host);
    c->host = NULL;
}

/* Drains packets until the server disconnects or the deadline passes.
   Returns 1 on disconnect. */
static int wait_disconnect(conn_t* c, double timeout_s, int* packets)
{
    double deadline = now_s() + timeout_s;
    for (;;) {
        packet_t p = {NULL, 0};
        int      rc = conn_poll(c, deadline, &p);
        if (rc == 1) {
            free(p.data);
            if (packets) {
                (*packets)++;
            }
            continue;
        }
        return rc == -1;
    }
}

/* waits for the next packet; on failure describes what happened in why */
static int expect_packet(conn_t* c, double timeout_s, packet_t* p, const char* what, char* why, size_t why_size)
{
    int rc = conn_poll(c, now_s() + timeout_s, p);
    if (rc == 1) {
        return 1;
    }
    if (rc == -1) {
        set_text(why, why_size, "disconnected with data %u while waiting for %s", c->disconnect_data, what);
    } else {
        set_text(why, why_size, "no %s within %.0f s", what, timeout_s);
    }
    return 0;
}

static int expect_type(packet_t* p, uint8_t type, const char* what, char* why, size_t why_size)
{
    uint8_t got;
    if (!p10_packet_type_of(p->data, p->size, &got) || got != type) {
        set_text(why, why_size, "expected %s (%u), got packet type %d (%zu bytes)", what, type,
                 p->size ? p->data[0] : -1, p->size);
        return 0;
    }
    return 1;
}

static int receive_greeting(conn_t* c, char* why, size_t why_size)
{
    packet_t     p;
    p10_greeting g;
    if (!expect_packet(c, 3.0, &p, "Greeting", why, why_size)) {
        return 0;
    }
    int ok = expect_type(&p, P10_PKT_GREETING, "Greeting", why, why_size);
    if (ok && !p10_decode_greeting(p.data, p.size, &g)) {
        set_text(why, why_size, "Greeting does not decode (%zu bytes)", p.size);
        ok = 0;
    }
    free(p.data);
    return ok;
}

static size_t build_initiate(uint8_t* buf, size_t cap, const char* protocol_name)
{
    static uint8_t          nonce[256];
    p10_writer              w;
    p10_initiate_connection in;
    for (size_t i = 0; i < sizeof nonce; i++) {
        nonce[i] = (uint8_t) (i * 37 + 11);
    }
    memset(&in, 0, sizeof in);
    in.protocol_name = p10_bytes_from_cstring(protocol_name);
    in.major         = P10_VERSION_MAJOR;
    in.minor         = P10_VERSION_MINOR;
    in.package       = p10_bytes_from_cstring("p10probe");
    in.environment   = p10_bytes_from_cstring("headless");
    in.locale        = p10_bytes_from_cstring("en_US");
    in.player_name   = p10_bytes_from_cstring("Probe");
    in.nonce.data    = nonce;
    in.nonce.len     = sizeof nonce;
    in.map_quality   = g_map_quality;
    p10_writer_init(&w, buf, cap);
    p10_encode_initiate_connection(&w, &in);
    return w.error ? 0 : w.size;
}

static void send_initiate(conn_t* c, const char* protocol_name)
{
    uint8_t buf[1024];
    size_t  n = build_initiate(buf, sizeof buf, protocol_name);
    conn_send(c, buf, n);
}

static void send_client_certificate(conn_t* c)
{
    uint8_t         buf[16];
    p10_writer      w;
    p10_certificate cert = {0, {NULL, 0}, {NULL, 0}};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_certificate(&w, P10_PKT_CLIENT_CERTIFICATE, &cert);
    conn_send(c, buf, w.size);
}

/* Walks every column of a VXL: a stream that ends exactly after the last
   column is a real map, not just bytes of the right size. */
static int vxl_valid(const uint8_t* v, size_t size, unsigned sx, unsigned sy)
{
    size_t pos = 0;
    for (size_t col = 0; col < (size_t) sx * sy; col++) {
        for (;;) {
            if (pos + 4 > size) {
                return 0;
            }
            uint8_t n = v[pos], s = v[pos + 1], e = v[pos + 2];
            if (n == 0) {
                if (e < s && e + 1 != s) {
                    return 0;
                }
                pos += 4u * (size_t) (e - s + 2);
                break;
            }
            pos += 4u * n;
        }
    }
    return pos == size;
}

static int inflate_all(const uint8_t* in, size_t in_size, uint8_t** out, size_t* out_size, size_t limit)
{
    z_stream z;
    size_t   cap = 1u << 20, used = 0;
    uint8_t* buf = (uint8_t*) malloc(cap);
    int      rc;
    memset(&z, 0, sizeof z);
    if (buf == NULL || inflateInit(&z) != Z_OK) {
        free(buf);
        return 0;
    }
    z.next_in  = (Bytef*) in;
    z.avail_in = (uInt) in_size;
    do {
        if (used == cap) {
            if (cap >= limit) {
                break;
            }
            uint8_t* bigger = (uint8_t*) realloc(buf, cap * 2);
            if (bigger == NULL) {
                break;
            }
            buf = bigger;
            cap *= 2;
        }
        z.next_out  = buf + used;
        z.avail_out = (uInt) (cap - used);
        rc          = inflate(&z, Z_NO_FLUSH);
        used        = cap - z.avail_out;
    } while (rc == Z_OK);
    inflateEnd(&z);
    if (rc != Z_STREAM_END) {
        free(buf);
        return 0;
    }
    *out      = buf;
    *out_size = used;
    return 1;
}

/* ---- scenarios ---- */

/* one full join asking for want_format ("vxl" or "ngmap") */
static void run_join(result_t* res, const char* want_format)
{
    conn_t   c;
    packet_t p;
    char*    o  = res->observed;
    size_t   os = sizeof res->observed;
    double   t0 = now_s();
    set_text(res->expected, sizeof res->expected,
             "full handshake, map-format=%s, map stream = map-bytes and decodes to a valid VXL, GameStateFinal, "
             "within 10 s",
             want_format);

    if (!conn_open(&c, P10_CONNECT_MAGIC, 3.0)) {
        set_text(o, os, "could not connect (disconnect data %u)", c.disconnect_data);
        conn_close(&c);
        return;
    }
    if (!receive_greeting(&c, o, os)) {
        conn_close(&c);
        return;
    }
    send_initiate(&c, P10_PROTOCOL_NAME);

    if (!expect_packet(&c, 3.0, &p, "ServerCertificate", o, os)) {
        conn_close(&c);
        return;
    }
    p10_certificate cert;
    int             ok = expect_type(&p, P10_PKT_SERVER_CERTIFICATE, "ServerCertificate", o, os);
    if (ok && !p10_decode_certificate(p.data, p.size, P10_PKT_SERVER_CERTIFICATE, &cert)) {
        set_text(o, os, "ServerCertificate does not decode");
        ok = 0;
    }
    free(p.data);
    if (!ok) {
        conn_close(&c);
        return;
    }
    send_client_certificate(&c);

    /* GameStateHeader: the four keys milestone 2 promises (7.1); ngmap may
       first be encoded in the background (1-2 s on a big map) */
    if (!expect_packet(&c, 8.0, &p, "GameStateHeader", o, os)) {
        conn_close(&c);
        return;
    }
    char       map_name[64] = "", map_format[16] = "", map_size[48] = "", map_bytes[24] = "";
    p10_reader r;
    p10_bytes  key, value;
    ok = expect_type(&p, P10_PKT_GAME_STATE_HEADER, "GameStateHeader", o, os);
    if (ok && !p10_begin_game_state_header(&r, p.data, p.size)) {
        set_text(o, os, "GameStateHeader does not decode");
        ok = 0;
    }
    while (ok && p10_next_map_entry(&r, &key, &value)) {
        if (p10_bytes_equal(key, "map-name")) {
            p10_bytes_copy(value, map_name, sizeof map_name);
        } else if (p10_bytes_equal(key, "map-format")) {
            p10_bytes_copy(value, map_format, sizeof map_format);
        } else if (p10_bytes_equal(key, "map-size")) {
            p10_bytes_copy(value, map_size, sizeof map_size);
        } else if (p10_bytes_equal(key, "map-bytes")) {
            p10_bytes_copy(value, map_bytes, sizeof map_bytes);
        }
    }
    if (ok && !p10_at_end(&r)) {
        set_text(o, os, "GameStateHeader malformed (error=%d, %zu of %zu bytes read)", r.error, r.pos, r.size);
        ok = 0;
    }
    free(p.data);
    unsigned sx = 0, sy = 0, sz = 0;
    char     tail;
    double header_at = now_s() - t0;
    if (ok && (!map_name[0] || strcmp(map_format, want_format) != 0 ||
               sscanf(map_size, "%u,%u,%u%c", &sx, &sy, &sz, &tail) != 3 || !map_bytes[0]))
    {
        set_text(o, os, "bad GameStateHeader: map-name=\"%s\" map-format=\"%s\" map-size=\"%s\" map-bytes=\"%s\"",
                 map_name, map_format, map_size, map_bytes);
        ok = 0;
    }
    if (!ok) {
        conn_close(&c);
        return;
    }
    unsigned long expected_bytes = strtoul(map_bytes, NULL, 10);

    /* MapData until MapDataFinal; other packets are allowed in between
       (TerrainUpdate during the transfer, 7.3) and only counted */
    size_t   cap = 1u << 20, total = 0;
    uint8_t* stream = (uint8_t*) malloc(cap);
    int      fragments = 0, others = 0, done = 0;
    double   deadline  = now_s() + 10.0;
    while (stream != NULL && !done) {
        int rc = conn_poll(&c, deadline, &p);
        if (rc != 1) {
            if (rc == -1) {
                set_text(o, os, "disconnected with data %u after %d fragments (%zu bytes)", c.disconnect_data,
                         fragments, total);
            } else {
                set_text(o, os, "map transfer stalled after %d fragments (%zu bytes)", fragments, total);
            }
            free(stream);
            conn_close(&c);
            return;
        }
        p10_map_data frag;
        uint8_t      type = 0;
        p10_packet_type_of(p.data, p.size, &type);
        if (type == P10_PKT_MAP_DATA) {
            if (!p10_decode_map_data(p.data, p.size, &frag)) {
                set_text(o, os, "MapData fragment %d does not decode", fragments);
                free(p.data);
                free(stream);
                conn_close(&c);
                return;
            }
            while (total + frag.fragment.len > cap) {
                uint8_t* bigger = (uint8_t*) realloc(stream, cap * 2);
                if (bigger == NULL) {
                    free(stream);
                    stream = NULL;
                    break;
                }
                stream = bigger;
                cap *= 2;
            }
            if (stream != NULL) {
                memcpy(stream + total, frag.fragment.data, frag.fragment.len);
                total += frag.fragment.len;
                fragments++;
            }
        } else if (type == P10_PKT_MAP_DATA_FINAL) {
            if (!p10_decode_empty(p.data, p.size, P10_PKT_MAP_DATA_FINAL)) {
                set_text(o, os, "MapDataFinal is not empty (%zu bytes)", p.size);
                free(p.data);
                free(stream);
                conn_close(&c);
                return;
            }
            done = 1;
        } else {
            others++;
        }
        free(p.data);
    }
    if (stream == NULL) {
        set_text(o, os, "out of memory collecting the map");
        conn_close(&c);
        return;
    }
    if (total != expected_bytes) {
        set_text(o, os, "map stream is %zu bytes, map-bytes says %lu", total, expected_bytes);
        free(stream);
        conn_close(&c);
        return;
    }
    uint8_t* vxl = NULL;
    size_t   vxl_size = 0;
    if (strcmp(map_format, "ngmap") == 0) {
        int w = 0, h = 0, dp = 0;
        int err = ngmap_decode(stream, total, &vxl, &vxl_size, &w, &h, &dp);
        if (err != NGMAP_OK) {
            set_text(o, os, "ngmap stream (%zu bytes) does not decode: %s", total, ngmap_strerror(err));
            free(stream);
            conn_close(&c);
            return;
        }
        if ((unsigned) w != sx || (unsigned) h != sy || (unsigned) dp != sz) {
            set_text(o, os, "ngmap decodes to %dx%dx%d, map-size says %ux%ux%u", w, h, dp, sx, sy, sz);
            free(vxl);
            free(stream);
            conn_close(&c);
            return;
        }
    } else {
        /* the largest VXL the server would allocate for this size (Session.c) */
        size_t limit = (size_t) sx * sy * (sz / 2) + (1u << 20);
        if (!inflate_all(stream, total, &vxl, &vxl_size, limit)) {
            set_text(o, os, "map stream (%zu bytes) is not a complete zlib stream", total);
            free(stream);
            conn_close(&c);
            return;
        }
    }
    free(stream);
    int vxl_ok = vxl_valid(vxl, vxl_size, sx, sy);
    free(vxl);
    if (!vxl_ok) {
        set_text(o, os, "map inflates to %zu bytes but is not a %ux%u VXL", vxl_size, sx, sy);
        conn_close(&c);
        return;
    }

    uint8_t    ack[4];
    p10_writer w;
    p10_writer_init(&w, ack, sizeof ack);
    p10_encode_empty(&w, P10_PKT_MAP_DATA_ACKNOWLEDGE);
    conn_send(&c, ack, w.size);

    if (!expect_packet(&c, 3.0, &p, "GameStateFinal", o, os)) {
        conn_close(&c);
        return;
    }
    char   props[320] = "";
    size_t used       = 0;
    ok                = expect_type(&p, P10_PKT_GAME_STATE_FINAL, "GameStateFinal", o, os);
    if (ok && !p10_begin_game_state_final(&r, p.data, p.size)) {
        set_text(o, os, "GameStateFinal does not decode");
        ok = 0;
    }
    if (ok) {
        printf("  GameStateFinal properties:\n");
        while (p10_next_map_entry(&r, &key, &value)) {
            printf("    %.*s = %.*s\n", (int) key.len, (const char*) key.data, (int) value.len,
                   (const char*) value.data);
            if (used < sizeof props) {
                int n = snprintf(props + used, sizeof props - used, "%s%.*s", used ? "," : "", (int) key.len,
                                 (const char*) key.data);
                used += n > 0 ? (size_t) n : 0;
            }
        }
        uint32_t entities = p10_read_count(&r);
        for (uint32_t i = 0; i < entities && !r.error; i++) {
            p10_entity_item item;
            if (!p10_next_entity_item(&r, &item)) {
                r.error = 1;
            }
        }
        uint32_t players = p10_read_count(&r);
        for (uint32_t i = 0; i < players && !r.error; i++) {
            p10_player_item item;
            if (!p10_next_player_item(&r, &item)) {
                r.error = 1;
            }
        }
        if (!p10_at_end(&r)) {
            set_text(o, os, "GameStateFinal malformed (error=%d, %zu of %zu bytes read)", r.error, r.pos, r.size);
            ok = 0;
        } else {
            double took = now_s() - t0;
            res->pass   = took < 10.0;
            set_text(o, os,
                     "map %s %ux%ux%u %s, %zu bytes in %d fragments (+%d other), VXL %zu bytes; "
                     "final: %u entities, %u players, props [%s]; header after %.2f s, %.2f s",
                     map_name, sx, sy, sz, map_format, total, fragments, others, vxl_size, entities, players, props,
                     header_at, took);
        }
    }
    free(p.data);
    conn_close(&c);
}

static void run_handshake(result_t* res)
{
    g_map_quality = 0;
    run_join(res, "vxl");
}

/* two ngmap joins: the first encodes (or shares an encoding), the second
   should come from the cache; both must decode to a valid VXL */
static void run_ngmap(result_t* res)
{
    result_t first, second;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    g_map_quality = 80;
    run_join(&first, "ngmap");
    if (first.pass) {
        run_join(&second, "ngmap");
    }
    g_map_quality = 0;
    res->pass = first.pass && second.pass;
    set_text(res->expected, sizeof res->expected, "%s (twice, quality 80)", first.expected);
    set_text(res->observed, sizeof res->observed, "1st: %s || 2nd: %s", first.observed,
             first.pass ? second.observed : "not run");
}

/* connects with the magic, gets the Greeting, then lets `act` misbehave and
   expects the server to disconnect with `reason` */
static void run_expect_disconnect(result_t* res, uint32_t connect_data, int wait_greeting, void (*act)(conn_t*),
                                  uint32_t reason, double timeout_s, double min_s)
{
    conn_t c;
    int    packets = 0;
    char*  o       = res->observed;
    size_t os      = sizeof res->observed;

    if (!conn_open(&c, connect_data, 3.0)) {
        if (c.disconnected) {
            /* refused during the connect itself: still a valid answer */
            res->pass = c.disconnect_data == reason && min_s == 0;
            set_text(o, os, "disconnected during connect with data %u", c.disconnect_data);
        } else {
            set_text(o, os, "could not connect");
        }
        conn_close(&c);
        return;
    }
    double t0 = now_s();
    if (wait_greeting && !receive_greeting(&c, o, os)) {
        conn_close(&c);
        return;
    }
    if (act != NULL) {
        act(&c);
    }
    if (!wait_disconnect(&c, timeout_s, &packets)) {
        set_text(o, os, "still connected after %.1f s (%d packets received)", now_s() - t0, packets);
        conn_close(&c);
        return;
    }
    double took = c.disconnect_at - t0;
    res->pass   = c.disconnect_data == reason && took >= min_s;
    set_text(o, os, "disconnected with data %u after %.2f s (%d other packets before it)", c.disconnect_data, took,
             packets);
    conn_close(&c);
}

static void act_mismatch(conn_t* c)
{
    send_initiate(c, "WorldOfSpades 0.1");
}

static void act_garbage(conn_t* c)
{
    uint8_t buf[1024];
    size_t  n = build_initiate(buf, sizeof buf, P10_PROTOCOL_NAME);
    /* cut inside the nonce: every field before it decodes, the rest is gone */
    conn_send(c, buf, n / 2);
}

static void act_early(conn_t* c)
{
    send_client_certificate(c);
}

static void run_mismatch(result_t* res)
{
    set_text(res->expected, sizeof res->expected, "disconnect data %u (ProtocolMismatch)",
             P10_DISCONNECT_PROTOCOL_MISMATCH);
    run_expect_disconnect(res, P10_CONNECT_MAGIC, 1, act_mismatch, P10_DISCONNECT_PROTOCOL_MISMATCH, 5.0, 0);
}

static void run_idle(result_t* res)
{
    set_text(res->expected, sizeof res->expected, "disconnect data %u (Timeout) after %.0f..%.0f s",
             P10_DISCONNECT_TIMEOUT, IDLE_MIN_S, IDLE_MAX_S);
    /* waiting up to IDLE_MAX_S already bounds it from above */
    run_expect_disconnect(res, P10_CONNECT_MAGIC, 0, NULL, P10_DISCONNECT_TIMEOUT, IDLE_MAX_S, IDLE_MIN_S);
}

static void run_garbage(result_t* res)
{
    set_text(res->expected, sizeof res->expected, "disconnect data %u (MalformedPacket)",
             P10_DISCONNECT_MALFORMED_PACKET);
    run_expect_disconnect(res, P10_CONNECT_MAGIC, 1, act_garbage, P10_DISCONNECT_MALFORMED_PACKET, 5.0, 0);
}

static void run_early(result_t* res)
{
    set_text(res->expected, sizeof res->expected, "disconnect data %u (MalformedPacket)",
             P10_DISCONNECT_MALFORMED_PACKET);
    run_expect_disconnect(res, P10_CONNECT_MAGIC, 1, act_early, P10_DISCONNECT_MALFORMED_PACKET, 5.0, 0);
}

static void run_unknown(result_t* res)
{
    set_text(res->expected, sizeof res->expected, "disconnect data %u (vanilla wrong protocol)",
             VANILLA_WRONG_PROTO);
    run_expect_disconnect(res, UNKNOWN_CONNECT_DATA, 0, NULL, VANILLA_WRONG_PROTO, 5.0, 0);
}

static void run_vanilla(result_t* res)
{
    conn_t c;
    char*  o  = res->observed;
    size_t os = sizeof res->observed;
    /* the server speaks protocol 1.0 only: a 0.75 client must be refused the
       way a vanilla server refuses a version it does not know */
    set_text(res->expected, sizeof res->expected, "refused with data %u (server is 1.0 only)", VANILLA_WRONG_PROTO);
    int refused = !conn_open(&c, VANILLA_075, 3.0) || wait_disconnect(&c, 3.0, NULL);
    if (refused && c.disconnected) {
        res->pass = c.disconnect_data == VANILLA_WRONG_PROTO;
        set_text(o, os, "refused with data %u", c.disconnect_data);
    } else if (refused) {
        set_text(o, os, "could not connect");
    } else {
        set_text(o, os, "stayed connected: the server accepted 0.75");
    }
    conn_close(&c);
}

typedef struct {
    const char* name;
    void (*run)(result_t*);
} scenario_t;

static const scenario_t SCENARIOS[] = {
    {"handshake", run_handshake},
    {"ngmap", run_ngmap},
    {"mismatch", run_mismatch},
    {"idle", run_idle},
    {"garbage", run_garbage},
    {"early", run_early},
    {"vanilla", run_vanilla},
    {"unknown", run_unknown},
};
#define SCENARIO_COUNT (sizeof SCENARIOS / sizeof SCENARIOS[0])

static void usage(void)
{
    fprintf(stderr, "usage: p10probe <host> <port> <scenario>\nscenarios:");
    for (size_t i = 0; i < SCENARIO_COUNT; i++) {
        fprintf(stderr, " %s", SCENARIOS[i].name);
    }
    fprintf(stderr, " all\n");
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        usage();
        return 2;
    }
    g_host   = argv[1];
    long port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        usage();
        return 2;
    }
    g_port = (uint16_t) port;

    int selected = 0, failed = 0;
    for (size_t i = 0; i < SCENARIO_COUNT; i++) {
        selected |= strcmp(argv[3], "all") == 0 || strcmp(argv[3], SCENARIOS[i].name) == 0;
    }
    if (!selected) {
        usage();
        return 2;
    }
    if (enet_initialize() != 0) {
        fprintf(stderr, "enet_initialize failed\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (size_t i = 0; i < SCENARIO_COUNT; i++) {
        if (strcmp(argv[3], "all") != 0 && strcmp(argv[3], SCENARIOS[i].name) != 0) {
            continue;
        }
        result_t res;
        memset(&res, 0, sizeof res);
        res.name = SCENARIOS[i].name;
        SCENARIOS[i].run(&res);
        printf("%s %-9s expected: %s | observed: %s\n", res.pass ? "PASS" : "FAIL", res.name, res.expected,
               res.observed);
        failed += !res.pass;
    }
    enet_deinitialize();
    return failed ? 1 : 0;
}
