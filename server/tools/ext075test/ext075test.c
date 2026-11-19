// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Unit test of the 0.75 server's protocol extensions
 * (server/Source/Server/Legacy/Ext075.c): it feeds ExtInfo answers,
 * Teamplay pings and damage, ticks the Player Properties, and reads every
 * packet sent back the way our client reads it (the client's ext075.c and
 * network.c's read_PacketPlayerProperties: the same offsets, lengths and
 * colour order). The server is a few players in a hash, their connections
 * a fake transport that keeps what was sent; the profile, the clock and
 * the event log are fakes. Run by run.py next to it (it builds this file
 * with Ext075.c and tomlc99).
 */
#include <Server/Legacy/Ext075.h>
#include <Server/Profile.h>
#include <Util/Nanos.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fakes ---- */

static server_profile_t profile = SERVER_PROFILE_075;
static uint64_t         clock_ns = 1000000000ULL;

server_profile_t server_profile(void)
{
    return profile;
}
uint64_t get_nanos(void)
{
    return clock_ns;
}
void log_print_with_time(const char* format, ...)
{
    va_list a;
    va_start(a, format);
    vprintf(format, a);
    va_end(a);
}
/* the last event's text, for the checks that only a log line shows */
static char last_evt[512];
void evt_log(const player_t* player, const char* event, const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(last_evt, sizeof last_evt, fmt, a);
    va_end(a);
    printf("EVT %s %s %s\n", player != NULL ? player->name : "console", event, last_evt);
}

/* Plugins.c's on_flashlight: refused while plugin_refuses is set */
static int plugin_refuses;
int plugins_on_flashlight(server_t* server, player_t* player, int on)
{
    (void) server, (void) player, (void) on;
    return !plugin_refuses;
}

uint32_t stream_left(stream_t* s)
{
    return s->length - s->pos;
}
void stream_skip(stream_t* s, uint32_t n)
{
    s->pos = s->pos + n > s->length ? s->length : s->pos + n;
}
uint8_t stream_read_u8(stream_t* s)
{
    ACCESS_CHECK(s, 1);
    return s->data[s->pos++];
}
float stream_read_f(stream_t* s)
{
    ACCESS_CHECK(s, 4);
    float f;
    memcpy(&f, s->data + s->pos, 4);
    s->pos += 4;
    return f;
}

/* every packet a connection got */
#define MAX_PKT 256
typedef struct
{
    uint8_t data[128];
    size_t  len;
} pkt_t;
typedef struct
{
    pkt_t pkts[MAX_PKT];
    int   n;
} inbox_t;

static int fake_send(conn_t* c, uint8_t channel, const uint8_t* data, size_t len, int reliable)
{
    inbox_t* box = c->handle;
    if (channel != 0 || !reliable || len > sizeof box->pkts[0].data || box->n >= MAX_PKT) {
        printf("FAIL: send channel=%u reliable=%d len=%zu\n", channel, reliable, len);
        exit(1);
    }
    memcpy(box->pkts[box->n].data, data, len);
    box->pkts[box->n++].len = len;
    return 0;
}
static void     fake_disconnect(conn_t* c, uint32_t r, conn_close_t h) { (void) c; (void) r; (void) h; }
static uint32_t fake_rtt(const conn_t* c) { (void) c; return 0; }
/* this test's connection keeps no queue: everything it sends arrives at once */
static size_t fake_pending(const conn_t* c) { (void) c; return 0; }
static const conn_transport_t fake = {"fake", fake_send, fake_disconnect, fake_rtt, fake_pending};

/* ---- the client's reading, as the client's ext075.c does it ---- */

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static float f32(const uint8_t* p)
{
    float f;
    memcpy(&f, p, 4);
    return f;
}

typedef struct
{
    int     player;
    float   x, y, z, life;
    uint8_t surfaces, rgb[3];
    char    label[130];
} ping_t;

/* ext075_read_teamplay's TP_PING case; d is past the packet id */
static int read_ping(const uint8_t* d, int len, ping_t* out)
{
    if (len < 1 || d[0] != 1)
        return 0;
    d++;
    len--;
    if (len < 22)
        return 0;
    out->player   = d[0];
    out->x        = f32(d + 1);
    out->y        = f32(d + 5);
    out->z        = f32(d + 9);
    out->life     = f32(d + 13);
    out->surfaces = d[17];
    out->rgb[0] = d[20], out->rgb[1] = d[19], out->rgb[2] = d[18]; /* blue, green, red on the wire */
    int n = len - 22;
    memcpy(out->label, d + 22, n);
    out->label[n] = 0;
    return 1;
}

/* ext075_read_damage */
static int read_damage(const uint8_t* d, int len, int* victim, int* amount)
{
    switch (len) {
        case 2: *amount = d[1]; break;
        case 3: *amount = (int16_t) (d[1] | d[2] << 8); break;
        case 5:
            *amount = (int32_t) ((uint32_t) d[1] | (uint32_t) d[2] << 8 | (uint32_t) d[3] << 16 |
                                 (uint32_t) d[4] << 24);
            break;
        default: return 0;
    }
    *victim = d[0];
    return 1;
}

#pragma pack(push, 1)
struct PacketPlayerProperties /* the client's network.h */
{
    uint8_t  subID;
    uint8_t  player_id;
    uint8_t  health;
    uint8_t  blocks;
    uint8_t  grenades;
    uint8_t  ammo_clip;
    uint8_t  ammo_reserved;
    uint32_t score;
};
#pragma pack(pop)

/* ---- the world ---- */

static server_t server;
static player_t players[4];
static conn_t   conns[4];
static inbox_t  boxes[4];

static void clear(void)
{
    for (int i = 0; i < 4; i++)
        boxes[i].n = 0;
}

static void add_player(int id, team_t team, int bot)
{
    player_t* p = &players[id];
    p->id       = id;
    p->team     = team;
    p->state    = STATE_READY;
    p->alive    = 1;
    p->hp       = 100;
    p->blocks   = 50;
    p->grenades = 3;
    p->weapon_clip    = 10;
    p->weapon_reserve = 50;
    p->kills          = id;
    p->protocol       = PROTOCOL_075;
    p->is_bot         = bot;
    snprintf(p->name, sizeof p->name, bot ? "Bot%d" : "P%d", id);
    if (!bot) {
        conns[id].transport = &fake;
        conns[id].handle    = &boxes[id];
        p->conn             = &conns[id];
    }
    HASH_ADD(hh, server.players, id, sizeof(uint8_t), p);
}

static void feed(void (*fn)(server_t*, player_t*, stream_t*), player_t* p, const uint8_t* bytes, size_t n)
{
    uint8_t  copy[256];
    stream_t s = {copy, (uint32_t) n, 0};
    memcpy(copy, bytes, n);
    fn(&server, p, &s);
}

/* what the client sends as its ping (ext075_send_ping), past the packet id */
static size_t client_ping(uint8_t* buf, float x, float y, float z, const char* label)
{
    memset(buf, 0, 23 + 64);
    buf[0] = 1;
    buf[1] = 255;
    memcpy(buf + 2, &x, 4);
    memcpy(buf + 6, &y, 4);
    memcpy(buf + 10, &z, 4);
    size_t n = strlen(label);
    memcpy(buf + 23, label, n);
    return 23 + n;
}

/* the packets of one id in a box */
static int count_id(int who, uint8_t id)
{
    int k = 0;
    for (int i = 0; i < boxes[who].n; i++)
        k += boxes[who].pkts[i].data[0] == id;
    return k;
}
static const pkt_t* first_id(int who, uint8_t id)
{
    for (int i = 0; i < boxes[who].n; i++)
        if (boxes[who].pkts[i].data[0] == id)
            return &boxes[who].pkts[i];
    return NULL;
}

int main(void)
{
    server.s_map.map.size_x = 512;
    server.s_map.map.size_y = 512;
    server.s_map.map.size_z = 64;
    server.protocol.color_team[0] = (color_t) {{.b = 255, .g = 10, .r = 20}};
    server.protocol.color_team[1] = (color_t) {{.b = 5, .g = 200, .r = 30}};
    add_player(0, TEAM_A, 0);
    add_player(1, TEAM_A, 0);
    add_player(2, TEAM_B, 0);
    add_player(3, TEAM_A, 1); /* a bot: no conn */

    /* the 1.0 profile: nothing at all */
    profile = SERVER_PROFILE_10;
    ext075_on_join(&server, &players[0]);
    CHECK(boxes[0].n == 0, "1.0 profile sent %d packets", boxes[0].n);
    profile = SERVER_PROFILE_075;

    char conf[] = "[ext075]\ncompass = false\nping_lifetime = 9.5\n";
    char err[128];
    toml_table_t* root = toml_parse(conf, err, sizeof err);
    CHECK(root != NULL, "toml: %s", err);
    ext075_configure(root);
    toml_free(root);

    /* ExtInfo: the eight, version 1 (Daytime and Weather, 0x33, joined the
       seven the test was written for) */
    for (int i = 0; i < 4; i++)
        ext075_on_join(&server, &players[i]);
    const uint8_t want_info[] = {60, 8, 0x00, 1, 0x20, 1, 0x30, 1, 0x32, 1, 0x33, 1, 0xC0, 1, 0xC1, 1, 0xC2, 1};
    CHECK(boxes[0].n == 1 && boxes[0].pkts[0].len == sizeof want_info &&
              memcmp(boxes[0].pkts[0].data, want_info, sizeof want_info) == 0,
          "ExtInfo bytes");
    CHECK(boxes[3].n == 0, "the bot got something");
    clear();

    /* before an answer: no damage marker, no ping accepted */
    ext075_on_damage(&server, &players[0], &players[2], 34);
    CHECK(boxes[0].n == 0, "damage before the answer");

    /* the answers, shaped as the client builds them; p1 only Teamplay,
       and it also lists one never offered */
    const uint8_t all[] = {7, 0x00, 1, 0x20, 1, 0x30, 1, 0x32, 1, 0xC0, 1, 0xC1, 1, 0xC2, 1};
    const uint8_t p1[]  = {2, 0x30, 1, 0x99, 1};
    feed(ext075_receive_info, &players[0], all, sizeof all);
    feed(ext075_receive_info, &players[1], p1, sizeof p1);
    feed(ext075_receive_info, &players[2], all, sizeof all);
    CHECK(ext075_agreed(&players[0], EXT075_DAMAGE_MARKERS) && ext075_agreed(&players[0], EXT075_TEAMPLAY) &&
              ext075_agreed(&players[0], EXT075_PLAYER_PROPERTIES),
          "p0 agreed set");
    CHECK(ext075_agreed(&players[1], EXT075_TEAMPLAY) && !ext075_agreed(&players[1], EXT075_DAMAGE_MARKERS) &&
              !ext075_agreed(&players[1], EXT075_PLAYER_PROPERTIES),
          "p1 agreed set");
    CHECK(!ext075_agreed(&players[3], EXT075_TEAMPLAY), "the bot agreed");

    /* Teamplay Config right after the answer: esp and pings, no compass,
       north (0, -1); the client wants at least 9 bytes past the sub */
    const pkt_t* c = first_id(0, 112);
    CHECK(c != NULL && c->len == 11 && c->data[1] == 0, "Config packet");
    if (c != NULL) {
        CHECK((c->data[2] & 7) == 3, "Config features %d", c->data[2]);
        CHECK(f32(c->data + 3) == 0.0f && f32(c->data + 7) == -1.0f, "Config north");
    }
    CHECK(count_id(1, 112) == 1, "p1 Config");
    /* Flashlight after the answer: Light Config 255 (the default beam),
       then Light State, empty (no light on: no byte of bits at all) */
    {
        const uint8_t want_cfg[] = {114, 2, 255, 60, 128, 255, 179, 128, 0};
        int           cfgs = 0, states = 0;
        for (int i = 0; i < boxes[0].n; i++) {
            const pkt_t* p = &boxes[0].pkts[i];
            if (p->data[0] == 114 && p->data[1] == 2)
                cfgs += p->len == sizeof want_cfg && memcmp(p->data, want_cfg, sizeof want_cfg) == 0;
            if (p->data[0] == 114 && p->data[1] == 1)
                states += p->len == 2;
        }
        CHECK(cfgs == 1 && states == 1, "Light Config %d, Light State %d", cfgs, states);
        CHECK(count_id(1, 114) == 0, "p1 got Flashlight without agreeing");
    }
    CHECK(ext075_agreed(&players[0], EXT075_FLASHLIGHT) && ext075_agreed(&players[0], EXT075_PLAYER_LIMIT) &&
              ext075_agreed(&players[0], EXT075_MESSAGE_TYPES) && ext075_agreed(&players[0], EXT075_KICK_REASON),
          "p0 agreed to the new ones");
    CHECK(!ext075_agreed(&players[1], EXT075_FLASHLIGHT) && !ext075_agreed(&players[1], EXT075_KICK_REASON),
          "p1 agreed to what it never listed");
    clear();

    /* a ping from p0: to p0 and p1 (team A, Teamplay), not p2 nor the bot */
    uint8_t buf[128];
    size_t  n = client_ping(buf, 300.5f, 200.25f, 40.0f, "Danger");
    feed(ext075_receive_teamplay, &players[0], buf, n);
    ping_t pg;
    for (int who = 0; who < 2; who++) {
        const pkt_t* p = first_id(who, 112);
        CHECK(p != NULL && read_ping(p->data + 1, (int) p->len - 1, &pg), "ping to p%d", who);
        if (p == NULL)
            continue;
        CHECK(pg.player == 0, "ping player %d", pg.player);
        CHECK(pg.x == 300.5f && pg.y == 200.25f && pg.z == 40.0f, "ping at %f %f %f", pg.x, pg.y, pg.z);
        CHECK(pg.life == 9.5f, "ping life %f", pg.life);
        /* the client's rgb[] is {r, g, b} after its swap */
        CHECK(pg.rgb[0] == 20 && pg.rgb[1] == 10 && pg.rgb[2] == 255, "ping colour %d %d %d", pg.rgb[0], pg.rgb[1],
              pg.rgb[2]);
        CHECK(strcmp(pg.label, "Danger") == 0, "ping label '%s'", pg.label);
    }
    CHECK(boxes[2].n == 0, "the enemy got the ping");
    clear();

    /* the double press (a second within 0.35 s) passes, a third is refused,
       one comes back after 200 ms */
    clock_ns += 100 * 1000000ULL;
    feed(ext075_receive_teamplay, &players[0], buf, n);
    CHECK(count_id(1, 112) == 1, "second ping refused");
    clear();
    feed(ext075_receive_teamplay, &players[0], buf, n);
    CHECK(count_id(1, 112) == 0, "third ping passed");
    clock_ns += 210 * 1000000ULL;
    feed(ext075_receive_teamplay, &players[0], buf, n);
    CHECK(count_id(1, 112) == 1, "ping after the refill refused");
    clear();

    /* a label: control bytes dropped, cut at 64 bytes outside a character */
    char long_label[80];
    long_label[0] = '\n';
    memset(long_label + 1, 'a', 63);
    long_label[64] = (char) 0xC3; /* 'e' acute, split by the 64 byte cut */
    long_label[65] = (char) 0xA9;
    long_label[66] = 0;
    clock_ns += 1000 * 1000000ULL;
    n = client_ping(buf, 10, 10, 10, long_label);
    feed(ext075_receive_teamplay, &players[0], buf, n);
    const pkt_t* lp = first_id(1, 112);
    CHECK(lp != NULL && read_ping(lp->data + 1, (int) lp->len - 1, &pg), "long label ping");
    if (lp != NULL)
        CHECK(strlen(pg.label) == 63 && strspn(pg.label, "a") == 63, "long label len %zu", strlen(pg.label));
    clear();

    /* a ping during a map load, out of the map, or short: dropped */
    clock_ns += 1000 * 1000000ULL;
    players[2].state = STATE_STARTING_MAP;
    n = client_ping(buf, 10, 10, 10, "");
    feed(ext075_receive_teamplay, &players[2], buf, n);
    players[2].state = STATE_READY;
    n = client_ping(buf, 600, 10, 10, "");
    feed(ext075_receive_teamplay, &players[2], buf, n);
    n = client_ping(buf, NAN, 10, 10, "");
    feed(ext075_receive_teamplay, &players[2], buf, n);
    feed(ext075_receive_teamplay, &players[2], buf, 10);
    CHECK(boxes[2].n == 0, "a dropped ping was relayed");
    /* p1 did not agree to Teamplay? it did; p1's ping goes to team A too */
    n = client_ping(buf, 1, 2, 3, "x");
    feed(ext075_receive_teamplay, &players[1], buf, n);
    CHECK(count_id(0, 112) == 1 && count_id(1, 112) == 1, "p1 ping relay");
    clear();

    /* Damage Markers: to the attacker only, width by the amount */
    struct
    {
        int    amount;
        size_t len;
    } dm[] = {{34, 3}, {255, 3}, {256, 4}, {-20, 4}, {70000, 6}, {-40000, 6}};
    for (size_t i = 0; i < sizeof dm / sizeof *dm; i++) {
        clear();
        ext075_on_damage(&server, &players[0], &players[2], dm[i].amount);
        const pkt_t* p = first_id(0, 96);
        int          victim, amount;
        CHECK(p != NULL && p->len == dm[i].len && read_damage(p->data + 1, (int) p->len - 1, &victim, &amount) &&
                  victim == 2 && amount == dm[i].amount,
              "damage %d", dm[i].amount);
        CHECK(boxes[2].n == 0 && boxes[1].n == 0, "damage %d to others", dm[i].amount);
    }
    clear();
    ext075_on_damage(&server, &players[1], &players[2], 10); /* p1 did not agree */
    ext075_on_damage(&server, NULL, &players[2], 10);        /* a fall */
    ext075_on_damage(&server, &players[0], &players[0], 10); /* itself */
    ext075_on_damage(&server, &players[0], &players[2], 0);
    for (int i = 0; i < 4; i++)
        CHECK(boxes[i].n == 0, "damage that should not go out, to p%d", i);
    ext075_on_damage(&server, &players[3], &players[2], 10); /* a bot attacker */
    CHECK(boxes[2].n == 0, "the bot's damage");

    /* Player Properties: the first tick gives p0 and p2 everyone; p1 did not
       agree; an enemy gets zeros for the health, blocks and grenades */
    clear();
    players[0].hp = 77;
    ext075_tick(&server);
    CHECK(count_id(0, 64) == 4 && count_id(2, 64) == 4 && count_id(1, 64) == 0 && boxes[3].n == 0,
          "first properties %d %d %d", count_id(0, 64), count_id(2, 64), count_id(1, 64));
    for (int who = 0; who <= 2; who += 2)
        for (int i = 0; i < boxes[who].n; i++) {
            const pkt_t* p = &boxes[who].pkts[i];
            if (p->data[0] != 64)
                continue;
            struct PacketPlayerProperties pp;
            CHECK(p->len - 1 == sizeof pp, "properties length %zu", p->len - 1);
            memcpy(&pp, p->data + 1, sizeof pp);
            player_t* s    = &players[pp.player_id];
            int       open = who == pp.player_id || players[who].team == s->team;
            CHECK(pp.subID == 0, "sub");
            CHECK(pp.health == (open ? (uint8_t) s->hp : 0) && pp.blocks == (open ? s->blocks : 0) &&
                      pp.grenades == (open ? s->grenades : 0),
                  "p%d sees p%d health %d", who, pp.player_id, pp.health);
            CHECK(pp.ammo_clip == 10 && pp.ammo_reserved == 50 && pp.score == s->kills, "ammo/score of p%d",
                  pp.player_id);
        }
    clear();
    ext075_tick(&server);
    CHECK(boxes[0].n == 0 && boxes[2].n == 0, "properties without a change");
    /* a change: held for 100 ms after the last send, then one packet */
    players[2].weapon_clip = 9;
    ext075_player_changed(&server, &players[2]);
    clock_ns += 50 * 1000000ULL;
    ext075_tick(&server);
    CHECK(count_id(0, 64) == 0, "sent within 100 ms");
    clock_ns += 60 * 1000000ULL;
    ext075_tick(&server);
    CHECK(count_id(0, 64) == 1 && count_id(2, 64) == 1, "change not sent after 100 ms");
    clear();

    /* a new map on the same connection: no second ExtInfo, a Config, and
       everyone's properties again */
    ext075_on_join(&server, &players[0]);
    CHECK(count_id(0, 60) == 0 && count_id(0, 112) == 1, "rejoin packets");
    ext075_tick(&server);
    CHECK(count_id(0, 64) == 4, "rejoin properties %d", count_id(0, 64));
    clear();

    /* the server's own ping (255, inf) and an ESP Mark */
    ext075_server_ping(&server, TEAM_B, 280, 255, 43, INFINITY, server.protocol.color_team[1], "Objective");
    const pkt_t* sp = first_id(2, 112);
    CHECK(sp != NULL && read_ping(sp->data + 1, (int) sp->len - 1, &pg) && pg.player == 255 && isinf(pg.life) &&
              strcmp(pg.label, "Objective") == 0,
          "server ping");
    CHECK(boxes[0].n == 0, "server ping to the other team");
    clear();
    ext075_mark(&server, TEAM_A, &players[2], 30.0f, 1, server.protocol.color_team[0], "Spotted");
    const pkt_t* mk = first_id(0, 112);
    CHECK(mk != NULL, "mark");
    if (mk != NULL) {
        const uint8_t* d = mk->data + 2; /* past the id and the sub */
        int            len = (int) mk->len - 2;
        CHECK(mk->data[1] == 2 && len >= 11 && d[0] == 2 && f32(d + 1) == 30.0f && d[6] == 1 && d[9] == 20 &&
                  d[8] == 10 && d[7] == 255 && len - 11 == 7 && memcmp(d + 11, "Spotted", 7) == 0,
              "mark bytes");
    }
    clear();

    /* ---- Flashlight ---- */
    {
        /* p0 asks for its light (the player id it writes is ignored): p0 and
           p2 agreed and are told, p1 did not */
        const uint8_t on[] = {0, 7, 1};
        feed(ext075_receive_flashlight, &players[0], on, sizeof on);
        const uint8_t want_on[] = {114, 0, 0, 1};
        for (int who = 0; who <= 2; who += 2) {
            const pkt_t* p = first_id(who, 114);
            CHECK(p != NULL && p->len == 4 && memcmp(p->data, want_on, 4) == 0, "Light on to p%d", who);
        }
        CHECK(count_id(1, 114) == 0, "Light to p1 (not agreed)");
        CHECK(ext075_light_on(0), "light not on");
        clear();
        /* the same again: nothing new to tell */
        feed(ext075_receive_flashlight, &players[0], on, sizeof on);
        CHECK(count_id(0, 114) == 0 && count_id(2, 114) == 0, "same light relayed");
        /* the joiner's Light State: bit 0 */
        ext075_on_join(&server, &players[2]); /* a new map on its connection */
        const pkt_t* st = NULL;
        for (int i = 0; i < boxes[2].n; i++)
            if (boxes[2].pkts[i].data[0] == 114 && boxes[2].pkts[i].data[1] == 1)
                st = &boxes[2].pkts[i];
        CHECK(st != NULL && st->len == 3 && st->data[2] == 1, "Light State with p0's light");
        clear();
        /* refused: not agreed, a server-only sub, dead, a plugin, a spectator */
        feed(ext075_receive_flashlight, &players[1], on, sizeof on);
        CHECK(strstr(last_evt, "not_agreed") != NULL, "p1's light: %s", last_evt);
        const uint8_t state_sub[] = {1, 0xFF};
        feed(ext075_receive_flashlight, &players[2], state_sub, sizeof state_sub);
        CHECK(strstr(last_evt, "server_only") != NULL, "a client's Light State: %s", last_evt);
        players[2].alive = 0;
        feed(ext075_receive_flashlight, &players[2], on, sizeof on);
        CHECK(strstr(last_evt, "why=dead") != NULL, "a dead light: %s", last_evt);
        players[2].alive = 1;
        plugin_refuses = 1;
        feed(ext075_receive_flashlight, &players[2], on, sizeof on);
        CHECK(strstr(last_evt, "why=plugin") != NULL, "plugin refusal: %s", last_evt);
        plugin_refuses = 0;
        CHECK(boxes[0].n == 0 && boxes[2].n == 0, "a refused light was relayed");
        /* off: told; Create Player / Kill Action / a map: off without a word */
        const uint8_t off[] = {0, 0, 0};
        feed(ext075_receive_flashlight, &players[0], off, sizeof off);
        CHECK(first_id(2, 114) != NULL && first_id(2, 114)->data[3] == 0, "Light off");
        feed(ext075_receive_flashlight, &players[0], on, sizeof on);
        clear();
        ext075_light_off(0);
        CHECK(!ext075_light_on(0) && boxes[0].n == 0 && boxes[2].n == 0, "light_off spoke");
        feed(ext075_receive_flashlight, &players[0], on, sizeof on);
        ext075_map_start(&server);
        CHECK(!ext075_light_on(0), "light survived the map");
        clear();
    }

    /* ---- Message Types and UTF-8 chat ---- */
    {
        uint8_t pk[600];
        size_t  len = ext075_chat_packet(pk, sizeof pk, &players[0], 33, 5, "Careful", 0);
        CHECK(len == 3 + 7 && pk[0] == 17 && pk[1] == 33 && pk[2] == 5 && memcmp(pk + 3, "Careful", 7) == 0,
              "warning to p0");
        len = ext075_chat_packet(pk, sizeof pk, &players[1], 33, 5, "Careful", 0);
        CHECK(len == 3 + 7 && pk[2] == 2, "warning lowered for p1 (type %d)", pk[2]);
        /* nobody reads UTF-8 yet: CP437 as it is */
        const char cp[] = {'c', 'a', 'f', (char) 0x82, 0}; /* "cafe" with CP437's e acute */
        len = ext075_chat_packet(pk, sizeof pk, &players[0], 1, 0, cp, 1);
        CHECK(len == 3 + 5 && pk[6] == 0x82 && pk[7] == 0, "CP437 to a non UTF-8 reader");
        /* p0's extended version answer says it reads UTF-8 */
        ext075_on_version(&server, &players[0]);
        const pkt_t* hs  = first_id(0, 31);
        const pkt_t* get = first_id(0, 33);
        CHECK(hs != NULL && hs->len == 5, "Handshake Init");
        CHECK(get != NULL && get->len == 4 && get->data[1] == 0 && get->data[2] == 1 && get->data[3] == 2,
              "extended Version Get");
        clear();
        ext075_on_version(&server, &players[0]);
        CHECK(boxes[0].n == 0, "asked twice");
        if (hs != NULL) {
            /* the box was cleared, but its bytes are still there */
            uint8_t answer[4];
            memcpy(answer, hs->data + 1, 4);
            feed(ext075_receive_handshake, &players[0], answer, 4);
            CHECK(strstr(last_evt, "ok=1") != NULL, "handshake: %s", last_evt);
            answer[0] ^= 1;
            feed(ext075_receive_handshake, &players[0], answer, 4);
            CHECK(strstr(last_evt, "ok=0") != NULL, "a wrong handshake: %s", last_evt);
        }
        /* one packet per property, each repeating the ones before: the last */
        const uint8_t ext[] = {'x', 0, 3 + 4, 0, 1, 5, 'T', 'e', 's', 't', 1, 5, 'p', 't', '_', 'b', 'r',
                               2,   4, 1,     0, 0, 0};
        feed(ext075_receive_version_ext, &players[0], ext, sizeof ext);
        CHECK(strstr(last_evt, "app=\"Test\" version=0.1.5 locale=pt_br features=1 utf8=1") != NULL, "version_ext: %s",
              last_evt);
        CHECK(ext075_utf8(&players[0]) && !ext075_utf8(&players[1]), "UTF-8 readers");
        /* CP437 text beyond ASCII goes to p0 as UTF-8 behind 0xFF */
        len = ext075_chat_packet(pk, sizeof pk, &players[0], 1, 0, cp, 1);
        CHECK(len == 3 + 1 + 5 + 1 && pk[3] == 0xFF && memcmp(pk + 4, "caf\xc3\xa9", 5) == 0, "UTF-8 to p0");
        len = ext075_chat_packet(pk, sizeof pk, &players[0], 1, 0, "plain", 1);
        CHECK(len == 3 + 6 && pk[3] == 'p', "ASCII needs no prefix");
        /* a Russian line from p2: the server keeps "??????", p0 gets the
           original, p1 the CP437 */
        const char ru[] = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"; /* Privet in Cyrillic */
        char       inside[128];
        ext075_chat_utf8_begin(&players[2], (const uint8_t*) ru, sizeof ru, inside, sizeof inside);
        CHECK(strcmp(inside, "??????") == 0, "CP437 of the Russian line '%s'", inside);
        len = ext075_chat_packet(pk, sizeof pk, &players[0], 2, 0, inside, 1);
        CHECK(len == 3 + 1 + 12 + 1 && pk[3] == 0xFF && memcmp(pk + 4, ru, 12) == 0, "Russian to p0");
        len = ext075_chat_packet(pk, sizeof pk, &players[1], 2, 0, inside, 1);
        CHECK(len == 3 + 7 && memcmp(pk + 3, "??????", 6) == 0, "Russian to p1");
        CHECK(ext075_utf8(&players[2]), "a UTF-8 writer reads UTF-8");
        ext075_chat_utf8_done();
        /* after the line, "??????" is just question marks */
        len = ext075_chat_packet(pk, sizeof pk, &players[0], 2, 0, inside, 1);
        CHECK(pk[3] == '?', "the original outlived its line");
        clear();
    }

    /* ---- Kick Reason ---- */
    CHECK(ext075_kick_reason(&players[0], "Bye: griefing"), "kick reason to p0");
    {
        const pkt_t* k = first_id(0, 17);
        CHECK(k != NULL && k->len == 3 + 13 && k->data[1] == 255 && k->data[2] == 2 &&
                  memcmp(k->data + 3, "Bye: griefing", 13) == 0,
              "kick reason bytes");
    }
    CHECK(!ext075_kick_reason(&players[1], "Bye") && boxes[1].n == 0, "kick reason to p1 (not agreed)");
    CHECK(!ext075_kick_reason(&players[3], "Bye"), "kick reason to a bot");
    clear();

    /* the 1.0 profile: none of it */
    profile = SERVER_PROFILE_10;
    CHECK(!ext075_kick_reason(&players[0], "Bye") && !ext075_utf8(&players[0]), "1.0 profile spoke 0.75");
    feed(ext075_receive_flashlight, &players[0], (const uint8_t[]) {0, 0, 1}, 3);
    CHECK(boxes[0].n == 0 && boxes[2].n == 0, "1.0 profile relayed a light");
    profile = SERVER_PROFILE_075;

    /* the slot freed: a new connection there is offered ExtInfo again */
    ext075_player_left(&server, 0);
    CHECK(!ext075_agreed(&players[0], EXT075_TEAMPLAY), "left still agreed");
    ext075_on_join(&server, &players[0]);
    CHECK(count_id(0, 60) == 1, "ExtInfo after leaving");

    printf(failures ? "ext075test: %d FAILED\n" : "ext075test: all passed\n", failures);
    return failures != 0;
}
