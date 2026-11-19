// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The 0.75 protocol extensions our server offers, as ZeroSpades speaks them
 * (docs/plan-075-server.md section 7): ExtInfo (60), BetterSpades' Player
 * Properties (0x00, packet 64), ZeroSpades' Damage Markers (0x20, packet 96)
 * and Teamplay (0x30, packet 112: Config, Ping, ESP Mark). The client's
 * reader is the client's ext075.c; this is the writer. They are two directions
 * of one wire and stay two files.
 *
 * Credit: the wire format and the rules of Damage Markers and Teamplay
 * (lengths, colours in blue-green-red order, a Duration of 0 removing and
 * +inf lasting, player 255 for the server's own ping, what is dropped during
 * a map load) are ZeroSpades', read from its Sources/Client/NetProtocol.h,
 * NetClient.cpp, Client.cpp and Teamplay.h
 * (https://github.com/zerospades/zerospades, GPL-3; Teamplay by Fran6nd and
 * the ZeroSpades developers, ZeroSpades a fork of OpenSpades by yvt; the
 * compass bar by siecvi). This is our own C, written to speak with the
 * clients that speak to ZeroSpades' servers.
 *
 * Also, written from the aosprotocol specification
 * (https://github.com/zerospades/aosprotocol: protocol075.md and
 * the extensions folder, by the ZeroSpades developers and the Ace of Spades
 * community): Flashlight (0x32, packet 114: Light, Light State, Light
 * Config), the packetless Player Limit (0xC0), Message Types (0xC1) and
 * Kick Reason (0xC2), UTF-8 chat (a 0xFF first byte) and the Extra Packets'
 * version handshake (31/32) and Version Get (33) with its extended answer
 * (packet 9 marked 'x'). Player Limit is announced but the profile keeps 32
 * players: the 0.75 World Update has 32 slots (LEGACY_MAX_PLAYERS).
 */
#include <Server/EventLog.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Plugins.h>
#include <Server/Profile.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <proto10/text.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKET_EXT_INFO 60
#define PACKET_EXT_BASE 0x40
#define PACKET_CHAT     17
#define PACKET_VERSION_GET 33

/* Flashlight's sub-packets and its default beam (the spec's example) */
enum { FL_LIGHT = 0, FL_STATE = 1, FL_CONFIG = 2 };
#define FL_EVERYONE 255 /* Light Config for every player without one */

/* the Version Get properties asked: application, locale, feature flags */
enum { VP_APPLICATION = 0, VP_LOCALE = 1, VP_FEATURES = 2 };
#define VP_FEATURE_UTF8 1u

enum { TP_CONFIG = 0, TP_PING = 1, TP_MARK = 2 };
enum { TP_ESP = 1, TP_PING_OK = 2, TP_COMPASS = 4 }; /* Config's feature bits */
#define TP_SERVER    255 /* the player of the server's own ping */
#define TP_LABEL_MAX 64  /* bytes of a label, the client keeps no more */
#define TP_PING_HEAD 23  /* sub + player + 4 f32 + surfaces + bgr + message id */
#define TP_MARK_HEAD 12  /* sub + player + f32 + surfaces + show + bgr + message id */
#define TP_SURFACES  7   /* world, map, compass */

/* Player Properties: at most one a player every 100 ms, only on a change */
#define PROPS_PERIOD (NANO_IN_MILLI * 100)
/* pings: a bucket of two, one back every 200 ms (5 a second sustained, the
   1.0 server's rate), so the client's double press for danger, which comes
   within 0.35 s of the first ping, is not refused */
#define PING_REFILL (NANO_IN_MILLI * 200)
#define PING_BURST  2

/* the bit of each extension in a player's agreed set */
enum {
    AG_PROPS    = 1,
    AG_DAMAGE   = 2,
    AG_TEAMPLAY = 4,
    AG_FLASH    = 8,
    AG_LIMIT    = 16,
    AG_MESSAGES = 32,
    AG_SKY      = 128,
    AG_KICK     = 64,
};

static struct
{
    uint8_t damage_markers, teamplay, player_properties;
    uint8_t team_esp, pings, compass;
    float   ping_lifetime;
    uint8_t flashlight, player_limit, message_types, kick_reason, utf8_chat, version_info;
    /* Daytime and Weather: offered, and the time it says (night or day) */
    uint8_t daytime, night;
    /* the beam of every player (Light Config 255) */
    uint8_t flash_reach, flash_cone, flash_flicker;
    uint8_t flash_rgb[3];
} cfg = {1, 1, 1, 1, 1, 1, 12.0f, 1, 1, 1, 1, 1, 1, 1, 0, 60, 128, 0, {255, 179, 128}};

/* whose light is on (Flashlight): what a joining client is told */
static uint8_t lights[256];

/* the UTF-8 original of the line being handled (ext075_chat_utf8_begin) */
static struct
{
    uint8_t active;
    char    cp437[512];
    char    utf8[1024];
} chat_src;

typedef struct props
{
    uint8_t  health, blocks, grenades, clip, reserve;
    uint32_t score;
} props_t;

static struct slot
{
    const conn_t* conn;     /* the connection ExtInfo was offered on, NULL none */
    uint8_t       answered; /* its answer came */
    uint8_t       agreed;   /* AG_* */
    uint8_t       need_all; /* send it every player's properties */
    uint8_t       team;     /* the team it had at the last properties pass */
    /* as a subject of Player Properties */
    uint8_t       sent_ok;  /* sent holds what the others last got */
    props_t       sent;
    uint64_t      sent_at;
    /* the pings it may still send now (PING_BURST) and when one came back */
    uint8_t       ping_tokens;
    uint64_t      ping_at;
    /* the Extra Packets: asked once a connection, what the answers said */
    uint8_t       version_asked;
    uint8_t       handshake; /* 0 not sent, 1 sent, 2 answered right, 3 wrong */
    uint32_t      challenge;
    uint8_t       utf8;      /* reads UTF-8 chat (feature flags, or a 0xFF line of its own) */
    char          locale[16];
} slots[256];

static int _client(const player_t* p)
{
    /* conn first: a bot's calloc'd protocol reads as 0.75 */
    return p != NULL && p->conn != NULL && p->protocol == PROTOCOL_075;
}

static void _send(player_t* p, const uint8_t* data, size_t len)
{
    if (p->conn == NULL) {
        return;
    }
    conn_send(p->conn, 0, data, len, 1);
}

static void _put_f32(uint8_t* at, float f)
{
    memcpy(at, &f, sizeof f); /* little endian, as every 0.75 float */
}

static void _put_u32(uint8_t* at, uint32_t v)
{
    at[0] = v & 0xFF;
    at[1] = (v >> 8) & 0xFF;
    at[2] = (v >> 16) & 0xFF;
    at[3] = (v >> 24) & 0xFF;
}

/* on the map, downloading nothing: what a ping or a marker means anything to */
static int _in_game(const player_t* p)
{
    return p->state == STATE_JOINING || p->state == STATE_SPAWNING || p->state == STATE_WAITING_FOR_RESPAWN ||
           p->state == STATE_READY;
}

/* a body the clients were told about */
static int _subject(const player_t* p)
{
    return (p->state == STATE_SPAWNING || p->state == STATE_WAITING_FOR_RESPAWN || p->state == STATE_READY) &&
           (p->team == TEAM_A || p->team == TEAM_B);
}

static int _team_ok(uint8_t team)
{
    return team == TEAM_A || team == TEAM_B;
}

int ext075_agreed(const player_t* player, uint8_t ext)
{
    if (!profile_is_075() || !_client(player)) {
        return 0;
    }
    const struct slot* s = &slots[player->id];
    if (s->conn != player->conn || !s->answered) {
        return 0;
    }
    switch (ext) {
        case EXT075_PLAYER_PROPERTIES:
            return (s->agreed & AG_PROPS) != 0;
        case EXT075_DAMAGE_MARKERS:
            return (s->agreed & AG_DAMAGE) != 0;
        case EXT075_TEAMPLAY:
            return (s->agreed & AG_TEAMPLAY) != 0;
        case EXT075_FLASHLIGHT:
            return (s->agreed & AG_FLASH) != 0;
        case EXT075_PLAYER_LIMIT:
            return (s->agreed & AG_LIMIT) != 0;
        case EXT075_MESSAGE_TYPES:
            return (s->agreed & AG_MESSAGES) != 0;
        case EXT075_KICK_REASON:
            return (s->agreed & AG_KICK) != 0;
        case EXT075_DAYTIME:
            return (s->agreed & AG_SKY) != 0;
        default:
            return 0;
    }
}

/* a label as the client can draw it: no control bytes, at most
   TP_LABEL_MAX bytes, never cut inside a UTF-8 character */
static size_t _label(char* out, const uint8_t* in, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k < TP_LABEL_MAX; i++) {
        if (in[i] < 32 || in[i] == 127) {
            continue;
        }
        out[k++] = (char) in[i];
    }
    /* a cut that left a lead byte without all its continuation bytes */
    size_t start = k;
    while (start > 0 && ((uint8_t) out[start - 1] & 0xC0) == 0x80) {
        start--;
    }
    if (start > 0) {
        uint8_t lead = (uint8_t) out[start - 1];
        size_t  need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (lead >= 0xC0 && k - (start - 1) < need) {
            k = start - 1;
        }
    }
    out[k] = 0;
    return k;
}

void ext075_configure(toml_table_t* root)
{
    if (!profile_is_075()) {
        return; /* 1.0 has its own ways, and its config no [ext075] */
    }
    toml_table_t* t = root != NULL ? toml_table_in(root, "ext075") : NULL;
    if (t != NULL) {
        struct
        {
            const char* key;
            uint8_t*    value;
        } flags[] = {
            {"damage_markers", &cfg.damage_markers},
            {"teamplay", &cfg.teamplay},
            {"player_properties", &cfg.player_properties},
            {"team_esp", &cfg.team_esp},
            {"pings", &cfg.pings},
            {"compass", &cfg.compass},
            {"flashlight", &cfg.flashlight},
            {"player_limit", &cfg.player_limit},
            {"message_types", &cfg.message_types},
            {"kick_reason", &cfg.kick_reason},
            {"utf8_chat", &cfg.utf8_chat},
            {"version_info", &cfg.version_info},
            {"daytime", &cfg.daytime},
            {"night", &cfg.night},
        };
        for (size_t i = 0; i < sizeof flags / sizeof *flags; i++) {
            toml_datum_t d = toml_bool_in(t, flags[i].key);
            if (d.ok) {
                *flags[i].value = d.u.b ? 1 : 0;
            }
        }
        /* the beam: bytes on the wire, so each one 0..255 */
        struct
        {
            const char* key;
            uint8_t*    value;
        } beam[] = {
            {"flashlight_reach", &cfg.flash_reach},
            {"flashlight_cone", &cfg.flash_cone},
            {"flashlight_flicker", &cfg.flash_flicker},
        };
        for (size_t i = 0; i < sizeof beam / sizeof *beam; i++) {
            toml_datum_t d = toml_int_in(t, beam[i].key);
            if (d.ok && d.u.i >= 0 && d.u.i <= 255) {
                *beam[i].value = (uint8_t) d.u.i;
            } else if (d.ok) {
                LOG_WARNING("[ext075] %s must be 0..255, keeping %u", beam[i].key, *beam[i].value);
            }
        }
        toml_array_t* rgb = toml_array_in(t, "flashlight_color");
        if (rgb != NULL) {
            uint8_t c[3];
            int     ok = toml_array_nelem(rgb) == 3;
            for (int i = 0; ok && i < 3; i++) {
                toml_datum_t d = toml_int_at(rgb, i);
                ok             = d.ok && d.u.i >= 0 && d.u.i <= 255;
                c[i]           = ok ? (uint8_t) d.u.i : 0;
            }
            if (ok) {
                memcpy(cfg.flash_rgb, c, 3);
            } else {
                LOG_WARNING("[ext075] flashlight_color must be [r, g, b], each 0..255");
            }
        }
        toml_datum_t d = toml_double_in(t, "ping_lifetime");
        if (!d.ok) {
            d = toml_int_in(t, "ping_lifetime");
            if (d.ok) {
                d.u.d = (double) d.u.i;
            }
        }
        /* 0 would remove the ping it came with; +inf (toml's inf) lasts */
        if (d.ok && d.u.d > 0.0) {
            cfg.ping_lifetime = (float) d.u.d;
        } else if (d.ok) {
            LOG_WARNING("[ext075] ping_lifetime must be above 0, keeping %.1f", cfg.ping_lifetime);
        }
    }
    LOG_INFO("0.75 extensions: player properties %s, damage markers %s, teamplay %s (esp %d, pings %d, compass %d, "
             "ping lifetime %.1f s)",
             cfg.player_properties ? "on" : "off",
             cfg.damage_markers ? "on" : "off",
             cfg.teamplay ? "on" : "off",
             cfg.team_esp,
             cfg.pings,
             cfg.compass,
             cfg.ping_lifetime);
    evt_log(NULL,
            "ext075",
            "action=config properties=%d damage=%d teamplay=%d esp=%d pings=%d compass=%d ping_lifetime=%.1f",
            cfg.player_properties,
            cfg.damage_markers,
            cfg.teamplay,
            cfg.team_esp,
            cfg.pings,
            cfg.compass,
            cfg.ping_lifetime);
    LOG_INFO("0.75 extensions: flashlight %s (reach %u, cone %u, colour %u,%u,%u, flicker %u), player limit %s, "
             "message types %s, kick reason %s, UTF-8 chat %s, version info %s",
             cfg.flashlight ? "on" : "off",
             cfg.flash_reach,
             cfg.flash_cone,
             cfg.flash_rgb[0],
             cfg.flash_rgb[1],
             cfg.flash_rgb[2],
             cfg.flash_flicker,
             cfg.player_limit ? "on" : "off",
             cfg.message_types ? "on" : "off",
             cfg.kick_reason ? "on" : "off",
             cfg.utf8_chat ? "on" : "off",
             cfg.version_info ? "on" : "off");
    evt_log(NULL,
            "ext075",
            "action=config flashlight=%d player_limit=%d message_types=%d kick_reason=%d utf8=%d version_info=%d",
            cfg.flashlight,
            cfg.player_limit,
            cfg.message_types,
            cfg.kick_reason,
            cfg.utf8_chat,
            cfg.version_info);
}

static void _send_config(player_t* player)
{
    uint8_t buf[11];
    buf[0] = PACKET_EXT_BASE + EXT075_TEAMPLAY;
    buf[1] = TP_CONFIG;
    buf[2] = (cfg.team_esp ? TP_ESP : 0) | (cfg.pings ? TP_PING_OK : 0) | (cfg.compass ? TP_COMPASS : 0);
    /* north is the top of the map (y down), as ZeroSpades' default */
    _put_f32(buf + 3, 0.0f);
    _put_f32(buf + 7, -1.0f);
    _send(player, buf, sizeof buf);
}

/* Light Config 255: the beam of every player, ours all alike */
static void _send_light_config(player_t* player)
{
    uint8_t buf[9] = {EXT075_PACKET_FLASHLIGHT,
                      FL_CONFIG,
                      FL_EVERYONE,
                      cfg.flash_reach,
                      cfg.flash_cone,
                      cfg.flash_rgb[0],
                      cfg.flash_rgb[1],
                      cfg.flash_rgb[2],
                      cfg.flash_flicker};
    _send(player, buf, sizeof buf);
}

/* Light State: one bit per player id, low bit first, after the State Data
   (ExtInfo and its answer come after it, and so does this) */
static void _send_light_state(player_t* player)
{
    uint8_t buf[2 + 256 / 8];
    int     last = -1;
    memset(buf, 0, sizeof buf);
    buf[0] = EXT075_PACKET_FLASHLIGHT;
    buf[1] = FL_STATE;
    for (int id = 0; id < 256; id++) {
        if (lights[id]) {
            buf[2 + id / 8] |= (uint8_t) (1u << (id % 8));
            last = id;
        }
    }
    /* an id past the end of the array is off: only what is needed */
    _send(player, buf, 2 + (last < 0 ? 0 : (size_t) (last / 8 + 1)));
}

/* Daytime and Weather's Sky: Time (0 night, else day: 720, noon),
   Reserved, Weather (unused in version 1) */
static void _send_sky(player_t* player)
{
    uint8_t buf[8] = {EXT075_PACKET_DAYTIME, 0, 0, 0, 0, 0, 0, 0};
    if (!cfg.night) {
        buf[2] = 720 & 0xFF;
        buf[3] = 720 >> 8;
    }
    _send(player, buf, sizeof buf);
    evt_log(player, "ext075", "action=sky night=%u", cfg.night);
}

int ext075_night(void)
{
    return cfg.night;
}

int ext075_set_night(server_t* server, int night, const char* why)
{
    if (!profile_is_075() || server == NULL || !cfg.daytime || (cfg.night != 0) == (night != 0)) {
        return 0;
    }
    cfg.night = night ? 1 : 0;
    LOG_INFO("0.75 extensions: it is %s now (%s)", cfg.night ? "night" : "day", why);
    evt_log(NULL, "ext075", "action=daytime night=%u why=%s", cfg.night, why);
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (ext075_agreed(p, EXT075_DAYTIME)) {
            _send_sky(p);
        }
    }
    return 1;
}

static void _flashlight_joined(player_t* player)
{
    _send_light_config(player);
    _send_light_state(player);
    int on = 0;
    for (int id = 0; id < 256; id++) {
        on += lights[id] != 0;
    }
    evt_log(player, "ext075", "action=light_state lit=%d", on);
}

void ext075_on_join(server_t* server, player_t* player)
{
    (void) server;
    if (!profile_is_075() || !_client(player)) {
        return;
    }
    struct slot* s = &slots[player->id];
    if (s->conn == player->conn) {
        /* a new map on the same connection: the client keeps what it agreed
           and would answer a second ExtInfo with nothing, so none is sent;
           the Config and every player's state go again */
        if (ext075_agreed(player, EXT075_TEAMPLAY)) {
            _send_config(player);
        }
        if (ext075_agreed(player, EXT075_FLASHLIGHT)) {
            _flashlight_joined(player);
        }
        if (ext075_agreed(player, EXT075_DAYTIME)) {
            _send_sky(player);
        }
        s->need_all = 1;
        return;
    }
    memset(s, 0, sizeof *s);
    s->conn        = player->conn;
    s->ping_tokens = PING_BURST;
    uint8_t buf[2 + 8 * 2];
    size_t  n = 2;
    if (cfg.player_properties) {
        buf[n++] = EXT075_PLAYER_PROPERTIES;
        buf[n++] = 1;
    }
    if (cfg.damage_markers) {
        buf[n++] = EXT075_DAMAGE_MARKERS;
        buf[n++] = 1;
    }
    if (cfg.teamplay) {
        buf[n++] = EXT075_TEAMPLAY;
        buf[n++] = 1;
    }
    if (cfg.flashlight) {
        buf[n++] = EXT075_FLASHLIGHT;
        buf[n++] = 1;
    }
    if (cfg.daytime) {
        buf[n++] = EXT075_DAYTIME;
        buf[n++] = 1;
    }
    if (cfg.player_limit) {
        buf[n++] = EXT075_PLAYER_LIMIT;
        buf[n++] = 1;
    }
    if (cfg.message_types) {
        buf[n++] = EXT075_MESSAGE_TYPES;
        buf[n++] = 1;
    }
    if (cfg.kick_reason) {
        buf[n++] = EXT075_KICK_REASON;
        buf[n++] = 1;
    }
    buf[0] = PACKET_EXT_INFO;
    buf[1] = (uint8_t) ((n - 2) / 2);
    /* an empty list too: it tells the client there is nothing */
    _send(player, buf, n);
    evt_log(player, "ext075", "action=offer offered=%u", buf[1]);
}

void ext075_receive_info(server_t* server, player_t* player, stream_t* data)
{
    (void) server;
    if (!profile_is_075() || !_client(player) || data == NULL) {
        return;
    }
    struct slot* s = &slots[player->id];
    if (s->conn != player->conn || s->answered) {
        /* nothing offered on this connection, or a second answer */
        evt_log(player, "ext075", "action=agree_ignored why=%s", s->conn != player->conn ? "not_offered" : "again");
        return;
    }
    if (stream_left(data) < 1) {
        return;
    }
    uint8_t count  = stream_read_u8(data);
    uint8_t agreed = 0;
    for (int i = 0; i < count && stream_left(data) >= 2; i++) {
        uint8_t id      = stream_read_u8(data);
        uint8_t version = stream_read_u8(data);
        if (version < 1) {
            continue;
        }
        /* only what was offered: an answer may name more */
        if (id == EXT075_PLAYER_PROPERTIES && cfg.player_properties) {
            agreed |= AG_PROPS;
        } else if (id == EXT075_DAMAGE_MARKERS && cfg.damage_markers) {
            agreed |= AG_DAMAGE;
        } else if (id == EXT075_TEAMPLAY && cfg.teamplay) {
            agreed |= AG_TEAMPLAY;
        } else if (id == EXT075_FLASHLIGHT && cfg.flashlight) {
            agreed |= AG_FLASH;
        } else if (id == EXT075_PLAYER_LIMIT && cfg.player_limit) {
            agreed |= AG_LIMIT;
        } else if (id == EXT075_MESSAGE_TYPES && cfg.message_types) {
            agreed |= AG_MESSAGES;
        } else if (id == EXT075_KICK_REASON && cfg.kick_reason) {
            agreed |= AG_KICK;
        } else if (id == EXT075_DAYTIME && cfg.daytime) {
            agreed |= AG_SKY;
        }
    }
    s->answered = 1;
    s->agreed   = agreed;
    s->need_all = (agreed & AG_PROPS) != 0;
    s->team     = player->team;
    char   list[96] = "";
    size_t list_len = 0;
/* a name that does not fit is cut, never written past the buffer */
#define LIST_ADD(name)                                                                          \
    do {                                                                                        \
        list_len += (size_t) snprintf(list + list_len, sizeof list - list_len, ",%s", name);    \
        if (list_len >= sizeof list) {                                                          \
            list_len = sizeof list - 1;                                                         \
        }                                                                                       \
    } while (0)
    if (agreed & AG_PROPS) {
        LIST_ADD("properties");
    }
    if (agreed & AG_DAMAGE) {
        LIST_ADD("damage");
    }
    if (agreed & AG_TEAMPLAY) {
        LIST_ADD("teamplay");
    }
    if (agreed & AG_FLASH) {
        LIST_ADD("flashlight");
    }
    if (agreed & AG_LIMIT) {
        LIST_ADD("player_limit");
    }
    if (agreed & AG_MESSAGES) {
        LIST_ADD("message_types");
    }
    if (agreed & AG_KICK) {
        LIST_ADD("kick_reason");
    }
    if (agreed & AG_SKY) {
        LIST_ADD("daytime");
    }
#undef LIST_ADD
    evt_log(player,
            "ext075",
            "action=agree player=%u agreed=%s count=%u",
            player->id,
            list[0] != 0 ? list + 1 : "none",
            count);
    if (agreed & AG_TEAMPLAY) {
        _send_config(player);
    }
    if (agreed & AG_FLASH) {
        _flashlight_joined(player);
    }
    /* the first Sky at once: a client that agreed draws no world until it */
    if (agreed & AG_SKY) {
        _send_sky(player);
    }
}

/* one Teamplay Ping into buf, the length written */
static size_t _ping_packet(uint8_t* buf,
                           uint8_t  player,
                           float    x,
                           float    y,
                           float    z,
                           float    life,
                           uint8_t  surfaces,
                           color_t  color,
                           const char* label,
                           size_t   label_len)
{
    buf[0] = PACKET_EXT_BASE + EXT075_TEAMPLAY;
    buf[1] = TP_PING;
    buf[2] = player;
    _put_f32(buf + 3, x);
    _put_f32(buf + 7, y);
    _put_f32(buf + 11, z);
    _put_f32(buf + 15, life);
    buf[19] = surfaces & TP_SURFACES;
    /* blue, green, red on this wire (the client reads {d[20], d[19], d[18]}) */
    buf[20] = color.b;
    buf[21] = color.g;
    buf[22] = color.r;
    buf[23] = 0; /* message id: none */
    memcpy(buf + 24, label, label_len);
    return 24 + label_len;
}

/* to every player of team (TEAM_SPECTATOR: everyone) that agreed to
   Teamplay and is on the map; how many got it */
static int _send_team(server_t* server, uint8_t team, const uint8_t* buf, size_t len)
{
    int       sent = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if ((team == TEAM_SPECTATOR || p->team == team) && _in_game(p) && ext075_agreed(p, EXT075_TEAMPLAY)) {
            _send(p, buf, len);
            sent++;
        }
    }
    return sent;
}

static void _relay_ping(server_t* server, player_t* player, stream_t* data)
{
    const char* why = NULL;
    if (stream_left(data) < TP_PING_HEAD - 1) {
        why = "short";
    }
    uint8_t surfaces = 0;
    float   x = 0, y = 0, z = 0;
    char    label[TP_LABEL_MAX + 1] = "";
    size_t  label_len               = 0;
    if (why == NULL) {
        stream_read_u8(data); /* player: the server says who */
        x = stream_read_f(data);
        y = stream_read_f(data);
        z = stream_read_f(data);
        stream_read_f(data);  /* life: the server's */
        surfaces = stream_read_u8(data);
        stream_skip(data, 3); /* colour: the server's */
        stream_read_u8(data); /* message id */
        label_len = _label(label, data->data + data->pos, stream_left(data));
    }
    uint64_t     now = get_nanos();
    struct slot* s   = &slots[player->id];
    if (s->ping_tokens < PING_BURST) {
        uint64_t back = (now - s->ping_at) / PING_REFILL;
        if (back > 0) {
            s->ping_tokens = back >= (uint64_t) (PING_BURST - s->ping_tokens) ? PING_BURST : s->ping_tokens + (uint8_t) back;
            s->ping_at += back * PING_REFILL;
        }
    }
    if (why != NULL) {
        /* short: nothing more to check */
    } else if (!cfg.pings) {
        why = "not_allowed";
    } else if (!_team_ok(player->team)) {
        why = "spectator";
    } else if (!_in_game(player)) {
        /* a place in the map being replaced means nothing in the next */
        why = "map_load";
    } else if (!(isfinite(x) && isfinite(y) && isfinite(z) && x >= 0.0f && y >= 0.0f &&
                 x < server->s_map.map.size_x && y < server->s_map.map.size_y && z >= -64.0f && z <= 64.0f))
    {
        why = "position";
    } else if (s->ping_tokens == 0) {
        why = "rate";
    }
    if (why != NULL) {
        evt_log(player, "ext075", "action=ping_refused why=%s", why);
        return;
    }
    if (s->ping_tokens == PING_BURST) {
        s->ping_at = now; /* the refill clock starts with the first one spent */
    }
    s->ping_tokens--;
    uint8_t buf[24 + TP_LABEL_MAX];
    size_t  len = _ping_packet(buf,
                              player->id,
                              x,
                              y,
                              z,
                              cfg.ping_lifetime,
                              surfaces,
                              server->protocol.color_team[player->team],
                              label,
                              label_len);
    /* the pinger too: its client draws the echo in place of its own */
    int sent = _send_team(server, player->team, buf, len);
    evt_log(player,
            "ext075",
            "action=ping_relay at=%.1f,%.1f,%.1f team=%u life=%.1f label=\"%s\" sent=%d",
            x,
            y,
            z,
            player->team,
            cfg.ping_lifetime,
            label,
            sent);
}

void ext075_receive_teamplay(server_t* server, player_t* player, stream_t* data)
{
    if (!profile_is_075() || !_client(player) || data == NULL) {
        return;
    }
    if (!ext075_agreed(player, EXT075_TEAMPLAY)) {
        evt_log(player, "ext075", "action=ignored packet=teamplay why=not_agreed");
        return;
    }
    if (stream_left(data) < 1) {
        return;
    }
    uint8_t sub = stream_read_u8(data);
    if (sub == TP_PING) {
        _relay_ping(server, player, data);
        return;
    }
    /* a client sends only pings; a newer version's sub is skipped */
    evt_log(player, "ext075", "action=ignored packet=teamplay sub=%u why=unknown", sub);
}

void ext075_server_ping(server_t* server,
                        uint8_t   team,
                        float     x,
                        float     y,
                        float     z,
                        float     life,
                        color_t   color,
                        const char* label)
{
    if (!profile_is_075() || server == NULL || !cfg.teamplay || isnan(life) || life < 0.0f) {
        return;
    }
    char    text[TP_LABEL_MAX + 1];
    size_t  n = label != NULL ? _label(text, (const uint8_t*) label, strlen(label)) : 0;
    uint8_t buf[24 + TP_LABEL_MAX];
    text[n]    = 0;
    size_t len = _ping_packet(buf, TP_SERVER, x, y, z, life, 0, color, text, n);
    int    sent = _send_team(server, team, buf, len);
    evt_log(NULL,
            "ext075",
            "action=server_ping at=%.1f,%.1f,%.1f team=%u life=%.1f label=\"%s\" sent=%d",
            x,
            y,
            z,
            team,
            life,
            text,
            sent);
}

void ext075_mark(server_t* server,
                 uint8_t   team,
                 const player_t* target,
                 float     life,
                 uint8_t   show_name,
                 color_t   color,
                 const char* label)
{
    if (!profile_is_075() || server == NULL || target == NULL || !cfg.teamplay || isnan(life) || life < 0.0f) {
        return;
    }
    char    text[TP_LABEL_MAX + 1];
    size_t  n = label != NULL ? _label(text, (const uint8_t*) label, strlen(label)) : 0;
    uint8_t buf[1 + TP_MARK_HEAD + TP_LABEL_MAX];
    text[n] = 0;
    buf[0]  = PACKET_EXT_BASE + EXT075_TEAMPLAY;
    buf[1]  = TP_MARK;
    buf[2]  = target->id;
    _put_f32(buf + 3, life);
    buf[7]  = 0; /* surfaces: the client's own default */
    buf[8]  = show_name ? 1 : 0;
    buf[9]  = color.b;
    buf[10] = color.g;
    buf[11] = color.r;
    buf[12] = 0; /* message id */
    memcpy(buf + 13, text, n);
    int sent = _send_team(server, team, buf, 13 + n);
    evt_log(target, "ext075", "action=mark team=%u life=%.1f show_name=%u sent=%d", team, life, show_name, sent);
}

void ext075_on_damage(server_t* server, player_t* attacker, player_t* victim, int amount)
{
    (void) server;
    if (!profile_is_075() || attacker == NULL || victim == NULL || amount == 0) {
        return;
    }
    /* the attacker alone: the client draws a marker as its own hit (the hit
       marker and a number over the victim) and ignores one about itself, so
       the victim learns nothing from it and the attacker's teammates would
       see hits they never made. A fall, the water or the boundary has no
       attacker, and hurting oneself (a grenade) is no hit */
    if (attacker == victim || !ext075_agreed(attacker, EXT075_DAMAGE_MARKERS) || !_in_game(attacker)) {
        return;
    }
    uint8_t buf[6];
    size_t  len;
    buf[0] = PACKET_EXT_BASE + EXT075_DAMAGE_MARKERS;
    buf[1] = victim->id;
    /* the packet's length picks the amount's width */
    if (amount > 0 && amount <= 255) {
        buf[2] = (uint8_t) amount;
        len    = 3;
    } else if (amount >= INT16_MIN && amount <= INT16_MAX) {
        uint16_t v = (uint16_t) (int16_t) amount;
        buf[2]     = v & 0xFF;
        buf[3]     = v >> 8;
        len        = 4;
    } else {
        _put_u32(buf + 2, (uint32_t) amount);
        len = 6;
    }
    _send(attacker, buf, len);
    evt_log(attacker, "ext075", "action=damage victim=%u amount=%d bytes=%zu", victim->id, amount, len - 1);
}

static props_t _props_of(const player_t* p)
{
    props_t v;
    int     hp = p->alive ? p->hp : 0;
    v.health   = (uint8_t) (hp < 0 ? 0 : hp > 255 ? 255 : hp);
    v.blocks   = (uint8_t) (p->blocks > 255 ? 255 : p->blocks); /* the 0.75 byte */
    v.grenades = p->grenades;
    v.clip     = p->weapon_clip;
    v.reserve  = p->weapon_reserve;
    v.score    = p->kills + p->score; /* what the scoreboard shows, as the 1.0 bridge */
    return v;
}

static int _props_same(const props_t* a, const props_t* b)
{
    return a->health == b->health && a->blocks == b->blocks && a->grenades == b->grenades && a->clip == b->clip &&
           a->reserve == b->reserve && a->score == b->score;
}

/* subject's Player Properties as receiver may see them. The client keeps
   the ammo and the score of everyone (the ammo decides whether it draws
   another player's shot) but the health, blocks and grenades only of its
   own player: an enemy gets zeros there, nothing it could not see anyway */
static void _send_props(player_t* receiver, const player_t* subject, const props_t* v)
{
    int     open = receiver == subject || receiver->team == subject->team || receiver->team == TEAM_SPECTATOR;
    uint8_t buf[12];
    buf[0] = PACKET_EXT_BASE + EXT075_PLAYER_PROPERTIES;
    buf[1] = 0; /* sub 0 */
    buf[2] = subject->id;
    buf[3] = open ? v->health : 0;
    buf[4] = open ? v->blocks : 0;
    buf[5] = open ? v->grenades : 0;
    buf[6] = v->clip;
    buf[7] = v->reserve;
    _put_u32(buf + 8, v->score);
    _send(receiver, buf, sizeof buf);
}

static int _props_receiver(const player_t* p)
{
    return _in_game(p) && ext075_agreed(p, EXT075_PLAYER_PROPERTIES);
}

void ext075_player_changed(server_t* server, player_t* player)
{
    (void) server;
    if (!profile_is_075() || player == NULL || !cfg.player_properties) {
        return;
    }
    /* nothing to do now: the tick compares every body's figures with what
       the others last got (128 small structs, cheap), so a change the wire
       forgets to report still goes out, at most every PROPS_PERIOD */
}

void ext075_player_left(server_t* server, uint8_t id)
{
    (void) server;
    if (!profile_is_075()) {
        return;
    }
    memset(&slots[id], 0, sizeof slots[id]);
    lights[id] = 0; /* Player Left: every client turned it off */
}

void ext075_tick(server_t* server)
{
    if (!profile_is_075() || server == NULL || !cfg.player_properties) {
        return;
    }
    uint64_t  now = get_nanos();
    player_t *p, *tmp, *r, *rtmp;
    /* a receiver whose team changed sees its old and new mates differently */
    HASH_ITER(hh, server->players, r, rtmp)
    {
        if (_client(r) && slots[r->id].team != r->team) {
            slots[r->id].team     = r->team;
            slots[r->id].need_all = 1;
        }
    }
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_subject(p)) {
            slots[p->id].sent_ok = 0; /* sent again once it is back */
            continue;
        }
        struct slot* s = &slots[p->id];
        props_t      v = _props_of(p);
        int changed    = !s->sent_ok || !_props_same(&v, &s->sent);
        if (!changed || now - s->sent_at < PROPS_PERIOD) {
            continue;
        }
        HASH_ITER(hh, server->players, r, rtmp)
        {
            if (_props_receiver(r) && !slots[r->id].need_all) {
                _send_props(r, p, &v);
            }
        }
        s->sent    = v;
        s->sent_ok = 1;
        s->sent_at = now;
    }
    /* the ones that just agreed, came back from a map load or changed team */
    HASH_ITER(hh, server->players, r, rtmp)
    {
        if (!_client(r) || !slots[r->id].need_all || !_props_receiver(r)) {
            continue;
        }
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (_subject(p)) {
                props_t v = _props_of(p);
                _send_props(r, p, &v);
            }
        }
        slots[r->id].need_all = 0;
    }
}

/* ---- Flashlight ---- */

int ext075_light_on(uint8_t id)
{
    return lights[id] != 0;
}

void ext075_light_off(uint8_t id)
{
    lights[id] = 0;
}

void ext075_map_start(server_t* server)
{
    (void) server;
    if (!profile_is_075()) {
        return;
    }
    memset(lights, 0, sizeof lights);
}

int ext075_set_light(server_t* server, player_t* player, int on, const char* why)
{
    if (!profile_is_075() || server == NULL || player == NULL || !cfg.flashlight) {
        return 0;
    }
    on = on ? 1 : 0;
    if (lights[player->id] == on) {
        evt_log(player, "ext075", "action=light on=%d why=%s sent=0 same=1", on, why);
        return 0;
    }
    lights[player->id] = (uint8_t) on;
    /* the owner too: a client draws only a light the server switched on */
    uint8_t   buf[4] = {EXT075_PACKET_FLASHLIGHT, FL_LIGHT, player->id, (uint8_t) on};
    int       sent   = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_in_game(p) && ext075_agreed(p, EXT075_FLASHLIGHT)) {
            _send(p, buf, sizeof buf);
            sent++;
        }
    }
    evt_log(player, "ext075", "action=light on=%d why=%s sent=%d", on, why, sent);
    return 1;
}

void ext075_receive_flashlight(server_t* server, player_t* player, stream_t* data)
{
    if (!profile_is_075() || !_client(player) || data == NULL) {
        return;
    }
    if (!ext075_agreed(player, EXT075_FLASHLIGHT)) {
        evt_log(player, "ext075", "action=ignored packet=flashlight why=not_agreed");
        return;
    }
    if (stream_left(data) < 1) {
        return;
    }
    uint8_t sub = stream_read_u8(data);
    if (sub != FL_LIGHT) {
        /* a client only sends Light: the others are the server's */
        evt_log(player, "ext075", "action=ignored packet=flashlight sub=%u why=server_only", sub);
        return;
    }
    if (stream_left(data) < 2) {
        evt_log(player, "ext075", "action=light_refused why=short");
        return;
    }
    stream_read_u8(data); /* the player id: the sender's own, whatever it says */
    int         on  = stream_read_u8(data) != 0;
    const char* why = NULL;
    if (!_team_ok(player->team)) {
        why = "spectator";
    } else if (player->state != STATE_READY || !player->alive) {
        why = "dead";
    } else if (!plugins_on_flashlight(server, player, on)) {
        why = "plugin";
    }
    if (why != NULL) {
        /* a refused request is simply not relayed */
        evt_log(player, "ext075", "action=light_refused on=%d why=%s", on, why);
        return;
    }
    ext075_set_light(server, player, on, "request");
}

/* ---- Chat: Message Types, Kick Reason, UTF-8 ---- */

static int _beyond_ascii(const char* s)
{
    for (; *s != 0; s++) {
        if ((uint8_t) *s >= 0x80) {
            return 1;
        }
    }
    return 0;
}

int ext075_utf8(const player_t* player)
{
    if (!profile_is_075() || !_client(player) || !cfg.utf8_chat) {
        return 0;
    }
    const struct slot* s = &slots[player->id];
    /* OpenSpades read a 0xFF line as UTF-8 before the feature flags existed */
    return (s->conn == player->conn && s->utf8) || player->client == 'o';
}

size_t ext075_chat_packet(uint8_t*        buf,
                          size_t          cap,
                          const player_t* receiver,
                          uint8_t         sender,
                          uint8_t         type,
                          const char*     text,
                          int             with_nul)
{
    if (type > EXT075_CHAT_SYSTEM && (receiver == NULL || !ext075_agreed(receiver, EXT075_MESSAGE_TYPES))) {
        type = EXT075_CHAT_SYSTEM; /* what a client without Message Types shows */
    }
    size_t at = 0;
    buf[at++] = PACKET_CHAT;
    buf[at++] = sender;
    buf[at++] = type;
    size_t      room = cap - at - 2; /* the 0xFF and the NUL */
    const char* body = text;
    char        conv[1024];
    /* the line being relayed came in UTF-8: its own characters, Cyrillic
       too, which its CP437 turned into '?' */
    int original = chat_src.active && strcmp(text, chat_src.cp437) == 0;
    /* plain ASCII reads the same either way: the prefix only when needed */
    if (receiver != NULL && ext075_utf8(receiver) && (_beyond_ascii(text) || (original && _beyond_ascii(chat_src.utf8))))
    {
        if (original) {
            body = chat_src.utf8;
        } else {
            p10_cp437_to_utf8((const uint8_t*) text, strlen(text), conv, sizeof conv);
            body = conv;
        }
        buf[at++] = 0xFF;
    }
    size_t len = strlen(body);
    if (len > room) {
        len = room;
        /* never cut inside a UTF-8 character (CP437 has none to cut) */
        if (body != text) {
            while (len > 0 && ((uint8_t) body[len] & 0xC0) == 0x80) {
                len--;
            }
        }
    }
    memcpy(buf + at, body, len);
    at += len;
    if (with_nul) {
        buf[at++] = 0;
    }
    return at;
}

void ext075_chat_utf8_begin(player_t* sender, const uint8_t* utf8, size_t len, char* cp437, size_t cap)
{
    size_t n = 0;
    while (n < len && utf8[n] != 0) {
        n++;
    }
    p10_bytes in = {utf8, n};
    p10_utf8_to_cp437(in, cp437, cap);
    chat_src.active = 0;
    if (!profile_is_075()) {
        return;
    }
    if (n >= sizeof chat_src.utf8) {
        n = sizeof chat_src.utf8 - 1;
        while (n > 0 && (utf8[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(chat_src.utf8, utf8, n);
    chat_src.utf8[n] = 0;
    snprintf(chat_src.cp437, sizeof chat_src.cp437, "%s", cp437);
    chat_src.active = 1;
    /* a client that writes UTF-8 reads it */
    if (_client(sender) && slots[sender->id].conn == sender->conn) {
        slots[sender->id].utf8 = 1;
    }
    evt_log(sender, "ext075", "action=utf8_in bytes=%zu cp437=\"%s\"", n, cp437);
}

void ext075_chat_utf8_done(void)
{
    chat_src.active = 0;
}

int ext075_kick_reason(player_t* player, const char* text)
{
    if (!profile_is_075() || !_client(player) || text == NULL || text[0] == 0 ||
        !ext075_agreed(player, EXT075_KICK_REASON))
    {
        return 0;
    }
    uint8_t buf[3 + 2 + 255];
    size_t  len = ext075_chat_packet(buf, sizeof buf, player, EXT075_SERVER_ID, EXT075_CHAT_SYSTEM, text, 0);
    _send(player, buf, len);
    evt_log(player, "ext075", "action=kick_reason text=\"%s\"", text);
    return 1;
}

/* ---- Extra Packets: the version handshake and the version get ---- */

void ext075_on_version(server_t* server, player_t* player)
{
    (void) server;
    if (!profile_is_075() || !_client(player) || !cfg.version_info) {
        return;
    }
    struct slot* s = &slots[player->id];
    /* once a connection: a client that ignores the property list answers
       the second Get with a plain Version Response again */
    if (s->conn != player->conn || s->version_asked) {
        return;
    }
    s->version_asked = 1;
    /* not rand(): a seeded match must play the same whoever joins it */
    uint64_t x = get_nanos() ^ ((uint64_t) player->id << 32) ^ 0x9E3779B97F4A7C15ULL;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 29;
    s->challenge = (uint32_t) x;
    s->handshake = 1;
    uint8_t hs[5] = {EXT075_PACKET_HANDSHAKE_INIT};
    _put_u32(hs + 1, s->challenge);
    _send(player, hs, sizeof hs);
    uint8_t get[4] = {PACKET_VERSION_GET, VP_APPLICATION, VP_LOCALE, VP_FEATURES};
    _send(player, get, sizeof get);
    evt_log(player,
            "ext075",
            "action=version_ask client=%c version=%d.%d.%d challenge=%u",
            player->client > 32 && player->client < 127 ? player->client : '?',
            player->version_major,
            player->version_minor,
            player->version_revision,
            s->challenge);
}

void ext075_receive_handshake(server_t* server, player_t* player, stream_t* data)
{
    (void) server;
    if (!profile_is_075() || !_client(player) || data == NULL) {
        return;
    }
    struct slot* s = &slots[player->id];
    if (s->conn != player->conn || s->handshake == 0) {
        evt_log(player, "ext075", "action=handshake_ignored why=not_sent");
        return;
    }
    if (stream_left(data) < 4) {
        evt_log(player, "ext075", "action=handshake_ignored why=short");
        return;
    }
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) {
        c |= (uint32_t) stream_read_u8(data) << (8 * i);
    }
    s->handshake = c == s->challenge ? 2 : 3;
    evt_log(player, "ext075", "action=handshake ok=%d", s->handshake == 2);
}

void ext075_receive_version_ext(server_t* server, player_t* player, stream_t* data)
{
    (void) server;
    if (!profile_is_075() || !_client(player) || data == NULL || stream_left(data) < 1) {
        return;
    }
    struct slot* s = &slots[player->id];
    if (s->conn != player->conn || !s->version_asked) {
        evt_log(player, "ext075", "action=version_ext_ignored why=not_asked");
        return;
    }
    stream_read_u8(data); /* the 'x' */
    char     app[40]  = "";
    int      major = -1, minor = -1, revision = -1;
    uint32_t features = 0;
    int      have_features = 0, properties = 0;
    while (stream_left(data) >= 2) {
        uint8_t id  = stream_read_u8(data);
        uint8_t len = stream_read_u8(data);
        if (stream_left(data) < len) {
            break;
        }
        const uint8_t* p = data->data + data->pos;
        stream_skip(data, len);
        properties++;
        switch (id) {
            case VP_APPLICATION:
                if (len >= 3) {
                    major    = p[0];
                    minor    = p[1];
                    revision = p[2];
                    _label(app, p + 3, (size_t) len - 3 < sizeof app - 1 ? (size_t) len - 3 : sizeof app - 1);
                }
                break;
            case VP_LOCALE: {
                size_t n = len < sizeof s->locale - 1 ? len : sizeof s->locale - 1;
                _label(s->locale, p, n);
                break;
            }
            case VP_FEATURES:
                if (len >= 4) {
                    features      = (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 |
                               (uint32_t) p[3] << 24;
                    have_features = 1;
                }
                break;
            default:
                break; /* a property not asked for, or a newer one */
        }
    }
    if (have_features) {
        s->utf8 = (features & VP_FEATURE_UTF8) != 0;
    }
    evt_log(player,
            "ext075",
            "action=version_ext properties=%d app=\"%s\" version=%d.%d.%d locale=%s features=%u utf8=%d",
            properties,
            app,
            major,
            minor,
            revision,
            s->locale[0] ? s->locale : "-",
            features,
            s->utf8);
}
