// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Tests for common/proto10: golden vectors from docs/protocol-1.0.md, round
 * trips for every packet, the reference defects D1-D5, malformed input and a
 * small fuzzer. Build with the sanitizers (see CMakeLists.txt).
 */
#include "../codec.h"
#include "../orient.h"
#include "../text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                          \
    } while (0)

static void check_bytes(const char* name, const uint8_t* got, size_t got_len, const uint8_t* want, size_t want_len)
{
    checks++;
    if (got_len == want_len && memcmp(got, want, want_len) == 0)
        return;
    failures++;
    printf("FAIL %s\n  got: ", name);
    for (size_t i = 0; i < got_len; i++)
        printf("%02X ", got[i]);
    printf("\n want: ");
    for (size_t i = 0; i < want_len; i++)
        printf("%02X ", want[i]);
    printf("\n");
}

#define BUF_SIZE 8192
static uint8_t buf[BUF_SIZE];

static p10_writer new_writer(void)
{
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    return w;
}

static p10_bytes cs(const char* s)
{
    return p10_bytes_from_cstring(s);
}

static int vec3_eq(p10_vec3 a, p10_vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static int bytes_eq(p10_bytes a, p10_bytes b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

/* decodes any packet completely; returns 1 when it is well formed. used by the
   truncation tests and the fuzzer, so it must touch every decoder */
static int decode_any(const uint8_t* d, size_t n, int* items_out)
{
    uint8_t type;
    p10_reader r;
    int items = 0, forced;
    p10_terrain_cursor c;
    p10_bytes k, v;
    if (items_out)
        *items_out = 0;
    if (!p10_packet_type_of(d, n, &type))
        return 0;
    switch (type) {
        case P10_PKT_GREETING: {
            p10_greeting p;
            return p10_decode_greeting(d, n, &p);
        }
        case P10_PKT_INITIATE_CONNECTION: {
            p10_initiate_connection p;
            return p10_decode_initiate_connection(d, n, &p);
        }
        case P10_PKT_SERVER_CERTIFICATE:
        case P10_PKT_CLIENT_CERTIFICATE: {
            p10_certificate p;
            return p10_decode_certificate(d, n, type, &p);
        }
        case P10_PKT_KICK: {
            p10_kick p;
            return p10_decode_kick(d, n, &p);
        }
        case P10_PKT_GAME_STATE_HEADER:
            if (!p10_begin_game_state_header(&r, d, n))
                return 0;
            while (p10_next_map_entry(&r, &k, &v))
                items++;
            break;
        case P10_PKT_MAP_DATA: {
            p10_map_data p;
            return p10_decode_map_data(d, n, &p);
        }
        case P10_PKT_GAME_STATE_FINAL: {
            if (!p10_begin_game_state_final(&r, d, n))
                return 0;
            while (p10_next_map_entry(&r, &k, &v))
                items++;
            uint32_t ne = p10_read_count(&r);
            for (uint32_t i = 0; i < ne && !r.error; i++) {
                p10_entity_item it;
                if (!p10_next_entity_item(&r, &it))
                    r.error = 1;
                items++;
            }
            uint32_t np = p10_read_count(&r);
            for (uint32_t i = 0; i < np && !r.error; i++) {
                p10_player_item it;
                if (!p10_next_player_item(&r, &it))
                    r.error = 1;
                items++;
            }
            break;
        }
        case P10_PKT_GENERIC_COMMAND:
            if (!p10_begin_generic_command(&r, d, n))
                return 0;
            while (p10_next_generic_part(&r, &k))
                items++;
            break;
        case P10_PKT_MAP_DATA_ACKNOWLEDGE:
        case P10_PKT_MAP_DATA_FINAL:
            return p10_decode_empty(d, n, type);
        case P10_PKT_ENTITY_UPDATE:
        case P10_PKT_CLIENT_ENTITY_UPDATE: {
            p10_entity_item it;
            int ok = type == P10_PKT_ENTITY_UPDATE ? p10_begin_entity_update(&r, d, n, &forced)
                                                   : p10_begin_client_entity_update(&r, d, n);
            if (!ok)
                return 0;
            while (p10_next_entity_item(&r, &it))
                items++;
            break;
        }
        case P10_PKT_ENTITY_EVENT: {
            p10_entity_event p;
            return p10_decode_entity_event(d, n, &p);
        }
        case P10_PKT_ENTITY_DIE: {
            p10_entity_die p;
            int ok = p10_decode_entity_die(d, n, &p);
            /* the damage info is an optional tail: count it as an item so a cut
               right before it reads as "fewer items", which is valid */
            if (ok && items_out)
                *items_out = p.has_damage;
            return ok;
        }
        case P10_PKT_ENTITY_REMOVE: {
            p10_entity_remove p;
            return p10_decode_entity_remove(d, n, &p);
        }
        case P10_PKT_TERRAIN_UPDATE: {
            p10_map_edit e;
            if (!p10_begin_terrain_update(&r, d, n, &c))
                return 0;
            while (p10_next_map_edit(&r, &c, &e))
                items++;
            break;
        }
        case P10_PKT_PLAYER_UPDATE: {
            p10_player_item it;
            if (!p10_begin_player_update(&r, d, n))
                return 0;
            while (p10_next_player_item(&r, &it))
                items++;
            break;
        }
        case P10_PKT_PLAYER_REMOVE: {
            uint32_t id;
            if (!p10_begin_player_remove(&r, d, n))
                return 0;
            while (p10_next_player_id(&r, &id))
                items++;
            break;
        }
        case P10_PKT_PLAYER_ACTION: {
            p10_player_action p;
            return p10_decode_player_action(d, n, &p);
        }
        case P10_PKT_HIT_ENTITY: {
            p10_hit_entity p;
            return p10_decode_hit_entity(d, n, &p);
        }
        case P10_PKT_HIT_TERRAIN: {
            p10_hit_terrain p;
            return p10_decode_hit_terrain(d, n, &p);
        }
        case P10_PKT_DAMAGE: {
            p10_damage p;
            return p10_decode_damage(d, n, &p);
        }
        case P10_PKT_CHAT_MESSAGE: {
            p10_chat_message p;
            return p10_decode_chat_message(d, n, &p);
        }
        case P10_PKT_CHAT_BROADCAST: {
            p10_chat_broadcast p;
            return p10_decode_chat_broadcast(d, n, &p);
        }
        case P10_PKT_GAME_MODE_STATE:
            if (!p10_begin_game_mode_state(&r, d, n))
                return 0;
            while (p10_next_map_entry(&r, &k, &v))
                items++;
            break;
        case P10_PKT_BUILD_BLOCK: {
            p10_build_block p;
            return p10_decode_build_block(d, n, &p);
        }
        case P10_PKT_BUILD_LINE: {
            p10_build_line p;
            return p10_decode_build_line(d, n, &p);
        }
        case P10_PKT_THROW_GRENADE: {
            p10_throw_grenade p;
            return p10_decode_throw_grenade(d, n, &p);
        }
        default:
            return 0;
    }
    if (items_out)
        *items_out = items;
    return p10_at_end(&r);
}

/* packets whose payload ends with a list (or EntityDie's optional damage
   tail) may be cut cleanly between items */
static int is_list_packet(uint8_t type)
{
    return type == P10_PKT_GENERIC_COMMAND || type == P10_PKT_ENTITY_UPDATE ||
        type == P10_PKT_CLIENT_ENTITY_UPDATE || type == P10_PKT_TERRAIN_UPDATE || type == P10_PKT_PLAYER_UPDATE ||
        type == P10_PKT_PLAYER_REMOVE || type == P10_PKT_ENTITY_DIE;
}

/* every proper prefix of a valid packet must be rejected, or for list packets
   decode to fewer items; one extra byte must always be rejected */
static void check_malformed_variants(const char* name, const uint8_t* d, size_t n)
{
    static uint8_t tmp[BUF_SIZE + 1];
    int full_items;
    checks++;
    if (!decode_any(d, n, &full_items)) {
        failures++;
        printf("FAIL %s: valid packet rejected\n", name);
        return;
    }
    for (size_t len = 0; len < n; len++) {
        int items;
        memcpy(tmp, d, len);
        int ok = decode_any(tmp, len, &items);
        checks++;
        if (ok && !(is_list_packet(d[0]) && items < full_items)) {
            failures++;
            printf("FAIL %s: truncation to %zu bytes accepted\n", name, len);
            return;
        }
    }
    memcpy(tmp, d, n);
    tmp[n] = 0x00;
    checks++;
    /* lists may legitimately accept more bytes only if they form an item, and a
       lone 0x00 never does except as a PlayerRemove id or a generic part */
    if (decode_any(tmp, n + 1, NULL) && d[0] != P10_PKT_PLAYER_REMOVE && d[0] != P10_PKT_GENERIC_COMMAND) {
        failures++;
        printf("FAIL %s: trailing byte accepted\n", name);
    }
}

/* ---- golden vectors (docs/protocol-1.0.md) ---- */

static void test_golden(void)
{
    struct {
        uint64_t v;
        uint8_t b[3];
        size_t n;
    } vs[] = {{0, {0x00}, 1}, {127, {0x7F}, 1}, {128, {0x80, 0x01}, 2}, {300, {0xAC, 0x02}, 2}};
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++) {
        p10_writer w = new_writer();
        p10_write_varint(&w, vs[i].v);
        check_bytes("varint", buf, w.size, vs[i].b, vs[i].n);
        p10_reader r;
        p10_reader_init(&r, buf, w.size);
        CHECK(p10_read_varint(&r) == vs[i].v && p10_at_end(&r));
    }

    int64_t zs[]  = {0, -1, 1, -2};
    uint8_t zb[]  = {0x00, 0x01, 0x02, 0x03};
    for (int i = 0; i < 4; i++) {
        p10_writer w = new_writer();
        p10_write_svarint(&w, zs[i]);
        check_bytes("svarint", buf, w.size, &zb[i], 1);
        p10_reader r;
        p10_reader_init(&r, buf, w.size);
        CHECK(p10_read_svarint(&r) == zs[i]);
    }

    {
        uint8_t nonce[128] = {0};
        p10_greeting g     = {{nonce, sizeof nonce}};
        p10_writer w       = new_writer();
        p10_encode_greeting(&w, &g);
        uint8_t want[] = {0x01, 0x05, 'H', 'e', 'l', 'l', 'o', 0x80, 0x01};
        check_bytes("Greeting header", buf, sizeof want, want, sizeof want);
        CHECK(w.size == sizeof want + 128);
        check_malformed_variants("Greeting", buf, w.size);
    }

    {
        p10_terrain_cursor c;
        p10_map_edit e = {10, 20, 30, 100, {0xFF, 0x80, 0x00}, P10_BLOCK_CREATE_PLAYER};
        p10_writer w   = new_writer();
        p10_start_terrain_update(&w, &c);
        p10_put_map_edit(&w, &c, &e);
        uint8_t want[] = {0x28, 0x0A, 0x14, 0x1E, 0x64, 0xFF, 0x80, 0x00, 0x01};
        check_bytes("TerrainUpdate create", buf, w.size, want, sizeof want);
    }

    {
        p10_terrain_cursor c;
        p10_map_edit e = {300, 5, 63, 0, {0, 0, 0}, P10_BLOCK_DESTROY_DAMAGE};
        p10_writer w   = new_writer();
        p10_start_terrain_update(&w, &c);
        p10_put_map_edit(&w, &c, &e);
        uint8_t want[] = {0x28, 0x80, 0xAC, 0x02, 0x05, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x01};
        check_bytes("TerrainUpdate cursor", buf, w.size, want, sizeof want);
    }

    {
        /* rifle: fire 500 ms, reload 2500 ms, raise 500 ms, damage 100, magazine 10 */
        p10_entity_item it = {0};
        it.entity_id       = 1;
        it.mask            = P10_EU_CREATE;
        it.entity_type     = P10_ENT_PLAYER;
        for (int i = 0; i < 3; i++) {
            p10_weapon_params rifle = {0, 0, 0, P10_WEAPON_BULLET, 500, 2500, 500, 100, 10};
            it.weapon[i]            = rifle;
        }
        p10_writer w = new_writer();
        p10_put_entity_item(&w, &it);
        /* id, mask, type, 4 empty skins, then the first weapon */
        uint8_t want[] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4, 0x01, 0xC4, 0x09, 0xF4, 0x01, 0x64, 0x0A};
        check_bytes("Entity create with rifle", buf, sizeof want, want, sizeof want);
        CHECK(w.size == 7 + 3 * 9);
    }

    {
        p10_writer w = new_writer();
        p10_start_generic_command(&w);
        p10_put_generic_part(&w, "local-player");
        p10_put_generic_part(&w, "3");
        uint8_t want[] = {0x09, 0x0C, 'l', 'o', 'c', 'a', 'l', '-', 'p', 'l', 'a', 'y', 'e', 'r', 0x01, '3'};
        check_bytes("GenericCommand local-player", buf, w.size, want, sizeof want);
    }

    {
        p10_entity_remove p = {300};
        p10_writer w        = new_writer();
        p10_encode_entity_remove(&w, &p);
        uint8_t want[] = {0x21, 0xAC, 0x02};
        check_bytes("EntityRemove 300", buf, w.size, want, sizeof want);
    }
}

/* ---- primitives and rejections ---- */

static void test_primitives(void)
{
    p10_reader r;

    /* D1: 64-bit varints round trip, longer than 10 bytes is rejected */
    {
        uint64_t big[] = {UINT32_MAX, (uint64_t) UINT32_MAX + 1, UINT64_MAX};
        for (int i = 0; i < 3; i++) {
            p10_writer w = new_writer();
            p10_write_varint(&w, big[i]);
            p10_reader_init(&r, buf, w.size);
            CHECK(p10_read_varint(&r) == big[i] && p10_at_end(&r));
        }
        uint8_t overlong[11];
        memset(overlong, 0x80, sizeof overlong);
        p10_reader_init(&r, overlong, sizeof overlong);
        p10_read_varint(&r);
        CHECK(r.error);
        /* a 10th byte with more than the top bit is out of range */
        uint8_t too_big[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
        p10_reader_init(&r, too_big, sizeof too_big);
        p10_read_varint(&r);
        CHECK(r.error);
        /* 32-bit fields reject 2^32 */
        p10_writer w = new_writer();
        p10_write_varint(&w, (uint64_t) UINT32_MAX + 1);
        p10_reader_init(&r, buf, w.size);
        p10_read_varint32(&r);
        CHECK(r.error);
    }

    /* D2: strings with NUL are rejected, raw bytes are not */
    {
        uint8_t s[] = {0x03, 'a', 0x00, 'b'};
        p10_reader_init(&r, s, sizeof s);
        p10_read_string(&r);
        CHECK(r.error);
        p10_reader_init(&r, s, sizeof s);
        p10_bytes b = p10_read_bytes(&r);
        CHECK(!r.error && b.len == 3);
    }

    /* byte strings longer than 1 MiB are rejected before reading them */
    {
        p10_writer w = new_writer();
        p10_write_varint(&w, P10_MAX_BYTES + 1);
        p10_reader_init(&r, buf, w.size);
        p10_read_bytes(&r);
        CHECK(r.error);
    }

    /* writers fail instead of overflowing */
    {
        uint8_t small[4];
        p10_writer w;
        p10_writer_init(&w, small, sizeof small);
        p10_write_u32(&w, 1);
        CHECK(!w.error);
        p10_write_u8(&w, 1);
        CHECK(w.error && w.size == 4);
    }

    /* colour primitive is B, G, R */
    {
        p10_color c  = {1, 2, 3};
        p10_writer w = new_writer();
        p10_write_color(&w, c);
        uint8_t want[] = {3, 2, 1};
        check_bytes("color BGR", buf, w.size, want, sizeof want);
    }

    /* compact rotation keeps w >= 0 */
    {
        p10_vec3 q = p10_rotation_compact(0.5f, -0.5f, 0.5f, -0.5f);
        CHECK(q.x == -0.5f && q.y == 0.5f && q.z == -0.5f);
    }

    /* UTF-8 validation */
    {
        CHECK(p10_utf8_valid(cs("Deuce")));
        CHECK(p10_utf8_valid(cs("Jo\xc3\xa3o")));                 /* João */
        CHECK(p10_utf8_valid(cs("\xf0\x9f\x92\xa3")));            /* bomb emoji */
        CHECK(!p10_utf8_valid(cs("\xc3")));                        /* truncated */
        CHECK(!p10_utf8_valid(cs("\xc0\xaf")));                    /* overlong */
        CHECK(!p10_utf8_valid(cs("\xed\xa0\x80")));                /* surrogate */
        CHECK(!p10_utf8_valid(cs("\xf4\x90\x80\x80")));            /* > U+10FFFF */
        CHECK(!p10_utf8_valid(cs("\xff")));
    }

    /* bytes helpers */
    {
        char out[4];
        CHECK(p10_bytes_copy(cs("abc"), out, sizeof out) && !strcmp(out, "abc"));
        CHECK(!p10_bytes_copy(cs("abcd"), out, sizeof out) && out[0] == '\0');
        CHECK(p10_bytes_equal(cs("Hello"), "Hello") && !p10_bytes_equal(cs("Hell"), "Hello"));
    }
}

/* ---- round trips ---- */

static void test_session(void)
{
    uint8_t nonce[256];
    for (int i = 0; i < 256; i++)
        nonce[i] = (uint8_t) i;

    {
        p10_initiate_connection in = {cs(P10_PROTOCOL_NAME), 1, 0, 0, cs("BetterSpades"), cs("macOS arm64"),
                                      cs("pt_BR"), cs("Deuce"), {nonce, sizeof nonce}, 80};
        p10_initiate_connection out;
        p10_writer w = new_writer();
        p10_encode_initiate_connection(&w, &in);
        CHECK(!w.error);
        CHECK(p10_decode_initiate_connection(buf, w.size, &out));
        CHECK(p10_bytes_equal(out.protocol_name, P10_PROTOCOL_NAME) && out.major == 1 && out.minor == 0);
        CHECK(p10_bytes_equal(out.player_name, "Deuce") && p10_bytes_equal(out.locale, "pt_BR"));
        CHECK(out.nonce.len == 256 && out.nonce.data[255] == 255 && out.map_quality == 80);
        check_malformed_variants("InitiateConnection", buf, w.size);
    }

    for (int valid = 0; valid < 2; valid++) {
        p10_certificate in = {(uint8_t) valid, cs("cert"), cs("sig")}, out;
        p10_writer w       = new_writer();
        p10_encode_certificate(&w, P10_PKT_CLIENT_CERTIFICATE, &in);
        CHECK(p10_decode_certificate(buf, w.size, P10_PKT_CLIENT_CERTIFICATE, &out));
        CHECK(out.is_valid == valid);
        if (valid)
            CHECK(p10_bytes_equal(out.certificate, "cert") && p10_bytes_equal(out.signature, "sig"));
        /* the type must match the one the caller expects */
        CHECK(!p10_decode_certificate(buf, w.size, P10_PKT_SERVER_CERTIFICATE, &out));
        check_malformed_variants("Certificate", buf, w.size);
    }

    {
        p10_kick in = {cs("Server is restarting")}, out;
        p10_writer w = new_writer();
        p10_encode_kick(&w, &in);
        CHECK(p10_decode_kick(buf, w.size, &out) && p10_bytes_equal(out.reason, "Server is restarting"));
        check_malformed_variants("Kick", buf, w.size);
    }

    {
        uint8_t bad[] = {0x01, 0x05, 'H', 'e', 'l', 'l', 'x', 0x00};
        p10_greeting g;
        CHECK(!p10_decode_greeting(bad, sizeof bad, &g));
    }

    for (int t = P10_PKT_MAP_DATA_ACKNOWLEDGE; t <= P10_PKT_MAP_DATA_FINAL; t++) {
        p10_writer w = new_writer();
        p10_encode_empty(&w, (uint8_t) t);
        CHECK(w.size == 1 && p10_decode_empty(buf, w.size, (uint8_t) t));
        check_malformed_variants("empty", buf, w.size);
    }
}

static void test_game_state(void)
{
    p10_reader r;
    p10_bytes k, v;

    {
        p10_writer w = new_writer();
        p10_start_game_state_header(&w);
        p10_put_map_entry(&w, "map-name", "Pinpoint");
        p10_put_map_entry(&w, "", "skipped");
        p10_put_map_entry(&w, "map-format", "vxl");
        p10_put_map_entry(&w, "empty-value", "");
        p10_put_map_end(&w);
        CHECK(p10_begin_game_state_header(&r, buf, w.size));
        CHECK(p10_next_map_entry(&r, &k, &v) && p10_bytes_equal(k, "map-name") && p10_bytes_equal(v, "Pinpoint"));
        CHECK(p10_next_map_entry(&r, &k, &v) && p10_bytes_equal(k, "map-format") && p10_bytes_equal(v, "vxl"));
        CHECK(p10_next_map_entry(&r, &k, &v) && p10_bytes_equal(k, "empty-value") && v.len == 0);
        CHECK(!p10_next_map_entry(&r, &k, &v) && p10_at_end(&r));
        check_malformed_variants("GameStateHeader", buf, w.size);
    }

    {
        uint8_t frag[4096];
        for (size_t i = 0; i < sizeof frag; i++)
            frag[i] = (uint8_t) (i * 7);
        p10_map_data in = {{frag, sizeof frag}}, out;
        p10_writer w    = new_writer();
        p10_encode_map_data(&w, &in);
        CHECK(p10_decode_map_data(buf, w.size, &out) && bytes_eq(in.fragment, out.fragment));
        check_malformed_variants("MapData", buf, w.size);
    }

    {
        p10_entity_item e = {0};
        e.entity_id       = 7;
        e.mask            = P10_EU_CREATE | P10_EU_TRAJECTORY | P10_EU_HEALTH;
        e.entity_type     = P10_ENT_GRENADE;
        e.trajectory.type = P10_TRAJ_GRAVITY;
        e.health          = 1;
        p10_player_item p = {3, P10_PU_CREATE | P10_PU_SCORE, cs("Deuce"), 0, 12, 0, 0, 0, 0, 0, 0, 0};

        p10_writer w = new_writer();
        p10_start_game_state_final(&w);
        p10_put_map_entry(&w, "fog-distance", "96");
        p10_put_map_end(&w);
        p10_put_count(&w, 1);
        p10_put_entity_item(&w, &e);
        p10_put_count(&w, 1);
        p10_put_player_item(&w, &p);

        CHECK(p10_begin_game_state_final(&r, buf, w.size));
        CHECK(p10_next_map_entry(&r, &k, &v) && p10_bytes_equal(v, "96"));
        CHECK(!p10_next_map_entry(&r, &k, &v));
        CHECK(p10_read_count(&r) == 1);
        p10_entity_item e2;
        CHECK(p10_next_entity_item(&r, &e2) && e2.entity_id == 7 && e2.entity_type == P10_ENT_GRENADE);
        CHECK(p10_read_count(&r) == 1);
        p10_player_item p2;
        CHECK(p10_next_player_item(&r, &p2) && p2.player_id == 3 && p2.score == 12);
        CHECK(p10_at_end(&r));
        check_malformed_variants("GameStateFinal", buf, w.size);
    }
}

static p10_entity_item full_player_item(void)
{
    p10_entity_item it = {0};
    it.entity_id       = 5;
    it.mask            = P10_EU_KNOWN;
    it.entity_type     = P10_ENT_PLAYER;
    it.body_skin       = cs("class/commando");
    for (int i = 0; i < 3; i++) {
        it.weapon_skin[i]           = cs(i == 0 ? "rocket" : i == 1 ? "revolver" : "grenade");
        it.weapon[i].full_auto      = (uint8_t) (i == 1);
        it.weapon[i].slow_reload    = (uint8_t) (i == 2);
        it.weapon[i].penetrative    = 1;
        it.weapon[i].type           = (uint8_t) (i == 2 ? P10_WEAPON_GEAR : P10_WEAPON_BULLET);
        it.weapon[i].fire_interval_ms = (uint16_t) (100 + i);
        it.weapon[i].reload_ms      = 2500;
        it.weapon[i].raise_ms       = 300;
        it.weapon[i].max_damage     = 49;
        it.weapon[i].magazine       = (uint8_t) (i == 2 ? 4 : 30);
    }
    it.flags                 = P10_EF_PLAYER_CLIP | P10_EF_WEAPON_CLIP;
    it.trajectory.type       = P10_TRAJ_PLAYER;
    it.trajectory.origin     = (p10_vec3) {256.5f, 100.25f, 60.0f};
    it.trajectory.velocity   = (p10_vec3) {0.1f, 0.0f, -0.36f};
    it.trajectory.euler      = (p10_vec3) {0.0f, 1.5f, 0.0f};
    it.trajectory.has_parent = 1;
    it.trajectory.parent     = 300;
    it.input.flags           = P10_IN_TOOL_PRIMARY | P10_IN_SPRINT;
    it.input.stance          = P10_STANCE_PRONE;
    it.input.xmove           = -127;
    it.input.ymove           = 127;
    it.tool                  = P10_TOOL_WEAPON1;
    it.block_color           = (p10_color) {10, 20, 30};
    it.health                = 100;
    return it;
}

static int entity_items_equal(const p10_entity_item* a, const p10_entity_item* b)
{
    if (a->entity_id != b->entity_id || a->mask != b->mask || a->entity_type != b->entity_type)
        return 0;
    if (!bytes_eq(a->body_skin, b->body_skin))
        return 0;
    for (int i = 0; i < 3; i++) {
        if (!bytes_eq(a->weapon_skin[i], b->weapon_skin[i]) || memcmp(&a->weapon[i], &b->weapon[i], sizeof a->weapon[i]))
            return 0;
    }
    const p10_trajectory *ta = &a->trajectory, *tb = &b->trajectory;
    return a->flags == b->flags && ta->type == tb->type && vec3_eq(ta->origin, tb->origin) &&
        vec3_eq(ta->velocity, tb->velocity) && vec3_eq(ta->euler, tb->euler) && vec3_eq(ta->rotation, tb->rotation) &&
        vec3_eq(ta->angular_velocity, tb->angular_velocity) && ta->has_parent == tb->has_parent &&
        ta->parent == tb->parent && !memcmp(&a->input, &b->input, sizeof a->input) && a->tool == b->tool &&
        !memcmp(&a->block_color, &b->block_color, sizeof a->block_color) && a->health == b->health;
}

static void test_entities(void)
{
    p10_reader r;
    int forced;

    p10_entity_item player = full_player_item();
    p10_entity_item rocket = {0};
    rocket.entity_id       = 900;
    rocket.mask            = P10_EU_CREATE | P10_EU_TRAJECTORY;
    rocket.entity_type     = P10_ENT_ROCKET;
    rocket.trajectory.type = P10_TRAJ_LINEAR;
    rocket.trajectory.origin           = (p10_vec3) {1, 2, 3};
    rocket.trajectory.velocity         = (p10_vec3) {4, 5, 6};
    rocket.trajectory.rotation         = p10_rotation_compact(0.0f, 0.0f, 0.70710678f, 0.70710678f);
    rocket.trajectory.angular_velocity = (p10_vec3) {0, 0, 3.14f};
    p10_entity_item health_only = {0};
    health_only.entity_id       = 5;
    health_only.mask            = P10_EU_HEALTH;
    health_only.health          = 42;

    p10_entity_item items[] = {player, rocket, health_only};
    p10_writer w            = new_writer();
    p10_start_entity_update(&w, 1);
    for (int i = 0; i < 3; i++)
        p10_put_entity_item(&w, &items[i]);
    CHECK(!w.error);
    CHECK(p10_begin_entity_update(&r, buf, w.size, &forced) && forced == 1);
    for (int i = 0; i < 3; i++) {
        p10_entity_item got;
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&items[i], &got));
    }
    p10_entity_item none;
    CHECK(!p10_next_entity_item(&r, &none) && p10_at_end(&r));
    check_malformed_variants("EntityUpdate", buf, w.size);

    /* ClientSideEntityUpdate carries the same items */
    w = new_writer();
    p10_start_client_entity_update(&w);
    p10_entity_item mine = player;
    mine.mask            = P10_EU_TRAJECTORY | P10_EU_PLAYER_INPUT | P10_EU_TOOL | P10_EU_BLOCK_COLOR;
    p10_put_entity_item(&w, &mine);
    CHECK(p10_begin_client_entity_update(&r, buf, w.size));
    p10_entity_item got;
    CHECK(p10_next_entity_item(&r, &got) && got.mask == mine.mask && got.input.stance == P10_STANCE_PRONE);
    CHECK(!p10_next_entity_item(&r, &got) && p10_at_end(&r));
    check_malformed_variants("ClientSideEntityUpdate", buf, w.size);

    /* layout-changing values that cannot be skipped are rejected */
    {
        uint8_t unknown_bit[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, 0x80};
        CHECK(!decode_any(unknown_bit, sizeof unknown_bit, NULL));
        uint8_t bad_traj[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, P10_EU_TRAJECTORY, 0x05};
        CHECK(!decode_any(bad_traj, sizeof bad_traj, NULL));
        uint8_t bad_type[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, P10_EU_CREATE, P10_ENT_COUNT};
        CHECK(!decode_any(bad_type, sizeof bad_type, NULL));
        uint8_t bad_stance[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, P10_EU_PLAYER_INPUT, 0xC0, 0, 0};
        CHECK(!decode_any(bad_stance, sizeof bad_stance, NULL));
        /* bit 5 of the input flags is still unused */
        uint8_t bad_flag[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, P10_EU_PLAYER_INPUT, 0x20, 0, 0};
        CHECK(!decode_any(bad_flag, sizeof bad_flag, NULL));
    }

    /* revision 1.1: the jetpack input bit (docs/protocol-1.0.md 8.4) */
    {
        p10_entity_item jet = {0};
        jet.entity_id       = 5;
        jet.mask            = P10_EU_PLAYER_INPUT;
        jet.input.flags     = P10_IN_JETPACK | P10_IN_SPRINT;
        jet.input.stance    = P10_STANCE_CROUCH;
        jet.input.xmove     = -127;
        jet.input.ymove     = 127;
        w                   = new_writer();
        p10_start_client_entity_update(&w);
        p10_put_entity_item(&w, &jet);
        /* golden vector: id 5, mask PlayerInput, flags 0x18 | crouch << 6 */
        const uint8_t want[] = {P10_PKT_CLIENT_ENTITY_UPDATE, 0x05, 0x08, 0x58, 0x81, 0x7F};
        check_bytes("ClientSideEntityUpdate jetpack", buf, w.size, want, sizeof want);
        CHECK(p10_begin_client_entity_update(&r, buf, w.size));
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&jet, &got) &&
              (got.input.flags & P10_IN_JETPACK));
        CHECK(!p10_next_entity_item(&r, &got) && p10_at_end(&r));
        check_malformed_variants("ClientSideEntityUpdate jetpack", buf, w.size);

        /* the same bit in a forced server correction, with every input flag */
        jet.input.flags = P10_IN_TOOL_PRIMARY | P10_IN_TOOL_SECONDARY | P10_IN_CHAT | P10_IN_SPRINT | P10_IN_JETPACK;
        jet.input.stance = P10_STANCE_STAND;
        w = new_writer();
        p10_start_entity_update(&w, 1);
        p10_put_entity_item(&w, &jet);
        const uint8_t want_all[] = {P10_PKT_ENTITY_UPDATE, 0x01, 0x05, 0x08, 0x1F, 0x81, 0x7F};
        check_bytes("EntityUpdate all input flags", buf, w.size, want_all, sizeof want_all);
        CHECK(p10_begin_entity_update(&r, buf, w.size, &forced) && forced == 1);
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&jet, &got));

        /* the equipped jetpack is an entity flag (bit 3) */
        p10_entity_item eq = {0};
        eq.entity_id       = 7;
        eq.mask            = P10_EU_FLAGS;
        eq.flags           = P10_EF_PLAYER_CLIP | P10_EF_WEAPON_CLIP | P10_EF_JETPACK;
        w                  = new_writer();
        p10_start_entity_update(&w, 0);
        p10_put_entity_item(&w, &eq);
        const uint8_t want_eq[] = {P10_PKT_ENTITY_UPDATE, 0x00, 0x07, 0x02, 0x0B};
        check_bytes("EntityUpdate jetpack flag", buf, w.size, want_eq, sizeof want_eq);
        CHECK(p10_begin_entity_update(&r, buf, w.size, &forced) && forced == 0);
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&eq, &got) && (got.flags & P10_EF_JETPACK));
        check_malformed_variants("EntityUpdate jetpack flag", buf, w.size);
        /* bits 4-7 of the entity flags are still unknown */
        uint8_t bad_ef[] = {P10_PKT_ENTITY_UPDATE, 0, 0x07, P10_EU_FLAGS, 0x10};
        CHECK(!decode_any(bad_ef, sizeof bad_ef, NULL));
    }
}

/* the inventory model (revision 1.3): Weapon3 is the gear, skins are checked
   and the tool slot is bounded */
static void test_inventory(void)
{
    p10_reader r;
    int forced;

    /* a Player Create round-trips with Gear in weapon[2] */
    {
        p10_entity_item in = full_player_item();
        CHECK(in.weapon[2].type == P10_WEAPON_GEAR);
        p10_writer w = new_writer();
        p10_start_entity_update(&w, 0);
        p10_put_entity_item(&w, &in);
        CHECK(!w.error);
        CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
        p10_entity_item got;
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&in, &got));
        CHECK(got.weapon[2].type == 3 && got.weapon[2].type == P10_WEAPON_GEAR && got.weapon[2].magazine == 4);
        CHECK(got.weapon[0].type == P10_WEAPON_BULLET && got.weapon[1].type == P10_WEAPON_BULLET);
        CHECK(!p10_next_entity_item(&r, &got) && p10_at_end(&r));
        check_malformed_variants("EntityUpdate gear", buf, w.size);
    }

    /* a tool slot past Weapon3 does not exist */
    {
        p10_entity_item in = {0};
        in.entity_id       = 9;
        in.mask            = P10_EU_TOOL;
        in.tool            = 5;
        p10_writer w       = new_writer();
        p10_start_client_entity_update(&w);
        p10_put_entity_item(&w, &in);
        CHECK(!decode_any(buf, w.size, NULL));
        in.tool = P10_TOOL_WEAPON3;
        w       = new_writer();
        p10_start_client_entity_update(&w);
        p10_put_entity_item(&w, &in);
        CHECK(decode_any(buf, w.size, NULL));
    }

    /* skins: lowercase names and the path's signs only, 32 bytes at most */
    {
        static const struct {
            const char* skin;
            int ok;
        } cases[] = {
            {"weaponskin/smg", 1},
            {"", 1},
            {"class/commando", 1},
            {"Rifle", 0},
            {"grenade2", 1},
            {"../etc/passwd", 0},
            {"two words", 0},
            {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0}, /* 33 */
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            p10_entity_item in = full_player_item();
            in.weapon_skin[i % 3] = cs(cases[i].skin);
            p10_writer w          = new_writer();
            p10_start_entity_update(&w, 0);
            p10_put_entity_item(&w, &in);
            checks++;
            if (decode_any(buf, w.size, NULL) != cases[i].ok) {
                failures++;
                printf("FAIL skin '%s' %s\n", cases[i].skin, cases[i].ok ? "rejected" : "accepted");
            }
        }
        /* the body's skin follows the same rule */
        p10_entity_item in = full_player_item();
        in.body_skin       = cs("Upper");
        p10_writer w       = new_writer();
        p10_start_entity_update(&w, 0);
        p10_put_entity_item(&w, &in);
        CHECK(!decode_any(buf, w.size, NULL));
    }
}

static void test_players(void)
{
    p10_reader r;
    p10_player_item in[] = {
        {3, P10_PU_CREATE | P10_PU_FLAGS | P10_PU_SCORE, cs("Deuce"), P10_PF_ADMIN, 1000, 0, 0, 0, 0, 0, 0, 0},
        {4, P10_PU_SCORE, {NULL, 0}, 0, 5, 0, 0, 0, 0, 0, 0, 0},
    };
    p10_writer w = new_writer();
    p10_start_player_update(&w);
    for (int i = 0; i < 2; i++)
        p10_put_player_item(&w, &in[i]);
    /* wire order is Create, Flags, Score: id 3, mask 7, "Deuce", flags 1, score 1000 */
    uint8_t want[] = {0x32, 0x03, 0x07, 0x05, 'D', 'e', 'u', 'c', 'e', 0x01, 0xE8, 0x07};
    check_bytes("PlayerUpdate order", buf, sizeof want, want, sizeof want);
    CHECK(p10_begin_player_update(&r, buf, w.size));
    p10_player_item out;
    CHECK(p10_next_player_item(&r, &out) && out.player_id == 3 && p10_bytes_equal(out.name, "Deuce") &&
          out.flags == P10_PF_ADMIN && out.score == 1000);
    CHECK(p10_next_player_item(&r, &out) && out.player_id == 4 && out.mask == P10_PU_SCORE && out.score == 5);
    CHECK(!p10_next_player_item(&r, &out) && p10_at_end(&r));
    check_malformed_variants("PlayerUpdate", buf, w.size);

    /* the [1.0] bits: team, kills/deaths, ping after the reference fields */
    {
        p10_player_item full = {7, 0x3f, cs("Bot1"), 0, 3, P10_TEAM_2, 4, 2, 35, 0, 0, 0};
        w = new_writer();
        p10_start_player_update(&w);
        p10_put_player_item(&w, &full);
        /* id 7, mask 3F, "Bot1", flags 0, score 3, team 1, kills 4, deaths 2, ping 35 */
        uint8_t want_full[] = {0x32, 0x07, 0x3F, 0x04, 'B', 'o', 't', '1', 0x00, 0x03, 0x01, 0x04, 0x02, 0x23};
        check_bytes("PlayerUpdate 1.0 bits", buf, w.size, want_full, sizeof want_full);
        CHECK(p10_begin_player_update(&r, buf, w.size));
        CHECK(p10_next_player_item(&r, &out) && out.team == P10_TEAM_2 && out.kills == 4 && out.deaths == 2 &&
              out.ping_ms == 35 && out.score == 3);
        CHECK(!p10_next_player_item(&r, &out) && p10_at_end(&r));
        check_malformed_variants("PlayerUpdate full", buf, w.size);
        uint8_t bad_team[] = {0x32, 0x07, P10_PU_TEAM, 0x05};
        CHECK(!decode_any(bad_team, sizeof bad_team, NULL));
        uint8_t spectator[] = {0x32, 0x07, P10_PU_TEAM, 0xFF};
        CHECK(decode_any(spectator, sizeof spectator, NULL));
    }

    w = new_writer();
    p10_start_player_remove(&w);
    p10_put_player_id(&w, 3);
    p10_put_player_id(&w, 300);
    CHECK(p10_begin_player_remove(&r, buf, w.size));
    uint32_t id;
    CHECK(p10_next_player_id(&r, &id) && id == 3);
    CHECK(p10_next_player_id(&r, &id) && id == 300);
    CHECK(!p10_next_player_id(&r, &id) && p10_at_end(&r));
    check_malformed_variants("PlayerRemove", buf, w.size);
}

static void test_events(void)
{
    {
        p10_entity_event in = {5, P10_EV_RELOAD_WEAPON, (uint64_t) 1 << 40}, out;
        p10_writer w        = new_writer();
        p10_encode_entity_event(&w, &in);
        CHECK(p10_decode_entity_event(buf, w.size, &out) && out.entity_id == 5 && out.param == in.param);
        check_malformed_variants("EntityEvent", buf, w.size);
    }

    /* D3: a death without damage info must decode */
    {
        p10_entity_die in = {0}, out;
        in.entity_id      = 5;
        in.type           = P10_DEATH_PLAYER;
        p10_writer w      = new_writer();
        p10_encode_entity_die(&w, &in);
        CHECK(p10_decode_entity_die(buf, w.size, &out) && !out.has_damage && out.entity_id == 5);
        check_malformed_variants("EntityDie plain", buf, w.size);
    }

    for (int from = 0; from < 2; from++) {
        p10_entity_die in = {0}, out;
        in.entity_id      = 5;
        in.type           = P10_DEATH_PLAYER;
        in.param          = 1;
        in.has_damage     = 1;
        in.damage.damage_type     = P10_DMG_WEAPON1;
        in.damage.has_from_entity = (uint8_t) from;
        in.damage.from_entity     = 9;
        in.damage.fire_position   = (p10_vec3) {1, 2, 3};
        in.damage.hit_position    = (p10_vec3) {4, 5, 6};
        p10_writer w              = new_writer();
        p10_encode_entity_die(&w, &in);
        CHECK(p10_decode_entity_die(buf, w.size, &out) && out.has_damage && out.damage.damage_type == P10_DMG_WEAPON1);
        CHECK(out.damage.has_from_entity == from && (!from || out.damage.from_entity == 9));
        CHECK(vec3_eq(out.damage.hit_position, in.damage.hit_position));
        check_malformed_variants("EntityDie damage", buf, w.size);
    }

    {
        p10_entity_remove in = {123456}, out;
        p10_writer w         = new_writer();
        p10_encode_entity_remove(&w, &in);
        CHECK(p10_decode_entity_remove(buf, w.size, &out) && out.entity_id == 123456);
        check_malformed_variants("EntityRemove", buf, w.size);
    }

    {
        p10_player_action in = {P10_EV_JUMP, 0}, out;
        p10_writer w         = new_writer();
        p10_encode_player_action(&w, &in);
        CHECK(p10_decode_player_action(buf, w.size, &out) && out.type == P10_EV_JUMP);
        check_malformed_variants("PlayerAction", buf, w.size);
    }

    {
        p10_writer w = new_writer();
        p10_start_generic_command(&w);
        p10_put_generic_part(&w, "join");
        p10_put_generic_part(&w, "1");
        p10_put_generic_part(&w, "");
        p10_reader r;
        p10_bytes part;
        CHECK(p10_begin_generic_command(&r, buf, w.size));
        CHECK(p10_next_generic_part(&r, &part) && p10_bytes_equal(part, "join"));
        CHECK(p10_next_generic_part(&r, &part) && p10_bytes_equal(part, "1"));
        CHECK(p10_next_generic_part(&r, &part) && part.len == 0);
        CHECK(!p10_next_generic_part(&r, &part) && p10_at_end(&r));
        check_malformed_variants("GenericCommand", buf, w.size);
    }
}

static void test_terrain(void)
{
    /* a batch that needs several cursor moves, in both directions */
    p10_map_edit in[] = {
        {10, 20, 30, 100, {1, 2, 3}, P10_BLOCK_CREATE_PLAYER},
        {11, 20, 30, 0, {0, 0, 0}, P10_BLOCK_DESTROY_DAMAGE},
        {511, 511, 63, 255, {255, 255, 255}, P10_BLOCK_CREATE_UNSPECIFIED},
        {0, 0, 0, 1, {9, 8, 7}, P10_BLOCK_CREATE_PLAYER},
        {384, 400, 62, 0, {0, 0, 0}, P10_BLOCK_DESTROY_UNSPECIFIED},
        {385, 400, 62, 0, {0, 0, 0}, P10_BLOCK_DESTROY_COLLAPSE},
        {300, 400, 62, 50, {4, 5, 6}, P10_BLOCK_CREATE_PLAYER},
    };
    size_t n = sizeof in / sizeof in[0];
    p10_terrain_cursor c;
    p10_writer w = new_writer();
    p10_start_terrain_update(&w, &c);
    for (size_t i = 0; i < n; i++)
        p10_put_map_edit(&w, &c, &in[i]);
    CHECK(!w.error);

    p10_reader r;
    CHECK(p10_begin_terrain_update(&r, buf, w.size, &c));
    for (size_t i = 0; i < n; i++) {
        p10_map_edit e;
        CHECK(p10_next_map_edit(&r, &c, &e));
        CHECK(e.x == in[i].x && e.y == in[i].y && e.z == in[i].z && e.health == in[i].health && e.cause == in[i].cause);
        if (in[i].health)
            CHECK(!memcmp(&e.color, &in[i].color, sizeof e.color));
    }
    p10_map_edit e;
    CHECK(!p10_next_map_edit(&r, &c, &e) && p10_at_end(&r));
    check_malformed_variants("TerrainUpdate", buf, w.size);

    /* negative coordinates cannot be encoded */
    {
        p10_map_edit neg = {-1, 0, 0, 0, {0, 0, 0}, 0};
        w                = new_writer();
        p10_start_terrain_update(&w, &c);
        p10_put_map_edit(&w, &c, &neg);
        CHECK(w.error);
    }

    /* a cursor at INT32_MAX plus a positive delta must not overflow */
    {
        uint8_t over[] = {P10_PKT_TERRAIN_UPDATE, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00};
        CHECK(!decode_any(over, sizeof over, NULL));
    }
}

static void test_hits(void)
{
    {
        p10_hit_entity in = {5, P10_HIT_HEAD, P10_DMG_WEAPON1, {1, 2, 3}, {4, 5, 6}}, out;
        p10_writer w      = new_writer();
        p10_encode_hit_entity(&w, &in);
        CHECK(p10_decode_hit_entity(buf, w.size, &out) && out.hit_type == P10_HIT_HEAD &&
              vec3_eq(out.hit_position, in.hit_position));
        check_malformed_variants("HitEntity", buf, w.size);
    }

    {
        p10_hit_terrain in = {-5, 511, 63, P10_DMG_DIG, {1, 2, 3}, {4, 5, 6}}, out;
        p10_writer w       = new_writer();
        p10_encode_hit_terrain(&w, &in);
        CHECK(p10_decode_hit_terrain(buf, w.size, &out) && out.x == -5 && out.y == 511 && out.z == 63);
        check_malformed_variants("HitTerrain", buf, w.size);
    }

    /* D4: fromEntity is written and read back in the right place */
    for (int from = 0; from < 2; from++) {
        p10_damage in = {0}, out;
        in.entity_id                = 5;
        in.damage.damage_type       = P10_DMG_GRENADE;
        in.damage.has_from_entity   = (uint8_t) from;
        in.damage.from_entity       = 77;
        in.damage.fire_position     = (p10_vec3) {1, 2, 3};
        in.damage.hit_position      = (p10_vec3) {4, 5, 6};
        in.to_entity                = 5;
        in.amount                   = 49;
        p10_writer w                = new_writer();
        p10_encode_damage(&w, &in);
        CHECK(p10_decode_damage(buf, w.size, &out));
        CHECK(out.damage.has_from_entity == from && (!from || out.damage.from_entity == 77));
        CHECK(out.to_entity == 5 && out.amount == 49 && vec3_eq(out.damage.fire_position, in.damage.fire_position));
        check_malformed_variants("Damage", buf, w.size);
    }
}

/* ---- [1.0] packets of section 12.2 ---- */

/* field by field: these structs have padding, so memcmp would compare garbage */
static int coord_eq(p10_blockcoord a, p10_blockcoord b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static int color_eq(p10_color a, p10_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void test_chat(void)
{
    /* golden vectors (docs/protocol-1.0.md 12.2) */
    {
        p10_chat_message in = {P10_CHAT_TEAM, cs("gg")}, out;
        p10_writer w        = new_writer();
        p10_encode_chat_message(&w, &in);
        uint8_t want[] = {0x46, 0x01, 0x02, 'g', 'g'};
        check_bytes("ChatMessage team gg", buf, w.size, want, sizeof want);
        CHECK(p10_decode_chat_message(buf, w.size, &out) && out.channel == P10_CHAT_TEAM &&
              p10_bytes_equal(out.text, "gg"));
        check_malformed_variants("ChatMessage", buf, w.size);
    }
    {
        p10_chat_broadcast in = {3, P10_CHAT_ALL, cs("hi")}, out;
        p10_writer w          = new_writer();
        p10_encode_chat_broadcast(&w, &in);
        uint8_t want[] = {0x47, 0x03, 0x00, 0x02, 'h', 'i'};
        check_bytes("ChatBroadcast player", buf, w.size, want, sizeof want);
        CHECK(p10_decode_chat_broadcast(buf, w.size, &out) && out.from_player == 3 && out.channel == P10_CHAT_ALL &&
              p10_bytes_equal(out.text, "hi"));
        check_malformed_variants("ChatBroadcast", buf, w.size);
    }
    {
        p10_chat_broadcast in = {P10_CHAT_FROM_SERVER, P10_CHAT_SYSTEM, cs("Round over")}, out;
        p10_writer w          = new_writer();
        p10_encode_chat_broadcast(&w, &in);
        uint8_t want[] = {0x47, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x02, 0x0A,
                          'R',  'o',  'u',  'n',  'd',  ' ',  'o',  'v',  'e', 'r'};
        check_bytes("ChatBroadcast server", buf, w.size, want, sizeof want);
        CHECK(p10_decode_chat_broadcast(buf, w.size, &out) && out.from_player == P10_CHAT_FROM_SERVER &&
              out.channel == P10_CHAT_SYSTEM);
        check_malformed_variants("ChatBroadcast server", buf, w.size);
    }

    /* channels */
    {
        p10_chat_message m;
        p10_chat_broadcast b;
        uint8_t empty_team[] = {0x46, 0x01, 0x00};
        CHECK(p10_decode_chat_message(empty_team, sizeof empty_team, &m) && m.text.len == 0);
        uint8_t system_from_client[] = {0x46, 0x02, 0x00};
        CHECK(!p10_decode_chat_message(system_from_client, sizeof system_from_client, &m));
        uint8_t bad_channel[] = {0x46, 0xFF, 0x00};
        CHECK(!p10_decode_chat_message(bad_channel, sizeof bad_channel, &m));
        uint8_t system_ok[] = {0x47, 0x00, 0x02, 0x00};
        CHECK(p10_decode_chat_broadcast(system_ok, sizeof system_ok, &b));
        uint8_t channel3[] = {0x47, 0x00, 0x03, 0x00};
        CHECK(!p10_decode_chat_broadcast(channel3, sizeof channel3, &b));
        /* fromPlayer is 32 bits */
        uint8_t from_too_big[] = {0x47, 0x80, 0x80, 0x80, 0x80, 0x10, 0x00, 0x00};
        CHECK(!p10_decode_chat_broadcast(from_too_big, sizeof from_too_big, &b));
    }

    /* text: UTF-8, no NUL, at most 255 bytes */
    {
        p10_chat_message m;
        uint8_t utf8[] = {0x46, 0x00, 0x05, 'o', 'l', 0xC3, 0xA1, '!'};
        CHECK(p10_decode_chat_message(utf8, sizeof utf8, &m) && m.text.len == 5);
        uint8_t bad_utf8[] = {0x46, 0x00, 0x01, 0xFF};
        CHECK(!p10_decode_chat_message(bad_utf8, sizeof bad_utf8, &m));
        uint8_t overlong[] = {0x46, 0x00, 0x02, 0xC0, 0xAF};
        CHECK(!p10_decode_chat_message(overlong, sizeof overlong, &m));
        uint8_t cut_sequence[] = {0x47, 0x01, 0x00, 0x01, 0xC3};
        p10_chat_broadcast b;
        CHECK(!p10_decode_chat_broadcast(cut_sequence, sizeof cut_sequence, &b));
        uint8_t nul[] = {0x46, 0x00, 0x03, 'a', 0x00, 'b'};
        CHECK(!p10_decode_chat_message(nul, sizeof nul, &m));

        static char text[P10_CHAT_MAX_TEXT + 2];
        for (size_t len = P10_CHAT_MAX_TEXT; len <= P10_CHAT_MAX_TEXT + 1; len++) {
            memset(text, 'a', len);
            text[len]    = '\0';
            p10_writer w = new_writer();
            p10_write_u8(&w, P10_PKT_CHAT_MESSAGE);
            p10_write_u8(&w, P10_CHAT_ALL);
            p10_write_cstring(&w, text);
            CHECK(p10_decode_chat_message(buf, w.size, &m) == (len <= P10_CHAT_MAX_TEXT));
            w = new_writer();
            p10_write_u8(&w, P10_PKT_CHAT_BROADCAST);
            p10_write_varint(&w, 1);
            p10_write_u8(&w, P10_CHAT_ALL);
            p10_write_cstring(&w, text);
            CHECK(p10_decode_chat_broadcast(buf, w.size, &b) == (len <= P10_CHAT_MAX_TEXT));
        }

        /* the encoders refuse what the decoders would reject */
        p10_chat_message long_msg = {P10_CHAT_ALL, cs(text)}; /* 256 bytes */
        p10_writer w              = new_writer();
        p10_encode_chat_message(&w, &long_msg);
        CHECK(w.error && w.size == 0);
        p10_chat_message sys_msg = {P10_CHAT_SYSTEM, cs("x")};
        w                        = new_writer();
        p10_encode_chat_message(&w, &sys_msg);
        CHECK(w.error);
        uint8_t raw[] = {'a', 0xFF};
        p10_chat_broadcast bad_b = {1, P10_CHAT_ALL, {raw, sizeof raw}};
        w                        = new_writer();
        p10_encode_chat_broadcast(&w, &bad_b);
        CHECK(w.error);
        uint8_t with_nul[] = {'a', 0x00};
        p10_chat_broadcast nul_b = {1, P10_CHAT_ALL, {with_nul, sizeof with_nul}};
        w                        = new_writer();
        p10_encode_chat_broadcast(&w, &nul_b);
        CHECK(w.error);
        p10_chat_broadcast ch3 = {1, 3, cs("x")};
        w                      = new_writer();
        p10_encode_chat_broadcast(&w, &ch3);
        CHECK(w.error);
    }
}

static void test_game_mode_state(void)
{
    p10_reader r;
    p10_bytes k, v;

    /* golden vector */
    {
        p10_writer w = new_writer();
        p10_start_game_mode_state(&w);
        p10_put_map_entry(&w, P10_GM_MODE, "ctf");
        p10_put_map_entry(&w, P10_GM_TEAM1_SCORE, "3");
        p10_put_map_end(&w);
        uint8_t want[] = {0x50, 0x04, 'm', 'o', 'd', 'e', 0x03, 'c', 't', 'f', 0x0B, 't', 'e', 'a',
                          'm',  '1',  '-', 's', 'c', 'o', 'r',  'e', 0x01, '3', 0x00};
        check_bytes("GameModeState", buf, w.size, want, sizeof want);
        check_malformed_variants("GameModeState small", buf, w.size);
    }

    /* a full CTF snapshot, then a TC one, through the vocabulary */
    {
        char intel[64];
        p10_intel carried = {P10_INTEL_CARRIED, 0, 0, 0, 12};
        CHECK(p10_format_intel(&carried, intel, sizeof intel) && !strcmp(intel, "carried"));
        p10_writer w = new_writer();
        p10_start_game_mode_state(&w);
        p10_put_map_entry(&w, P10_GM_MODE, "ctf");
        p10_put_map_entry(&w, P10_GM_TEAM1_NAME, "Blue");
        p10_put_map_entry(&w, P10_GM_TEAM1_COLOR, "0,0,255");
        p10_put_map_entry(&w, P10_GM_TEAM1_SCORE, "2");
        p10_put_map_entry(&w, P10_GM_TEAM2_SCORE, "0");
        p10_put_map_entry(&w, P10_GM_CAPTURE_LIMIT, "10");
        p10_put_map_entry(&w, P10_GM_TEAM1_INTEL, intel);
        p10_put_map_entry(&w, P10_GM_TEAM2_INTEL, "dropped");
        p10_put_map_end(&w);
        CHECK(!w.error);
        CHECK(p10_begin_game_mode_state(&r, buf, w.size));
        int n = 0, intel_ok = 0;
        while (p10_next_map_entry(&r, &k, &v)) {
            p10_intel in;
            if (n == 0)
                CHECK(p10_bytes_equal(k, P10_GM_MODE) && p10_bytes_equal(v, "ctf"));
            if (p10_bytes_equal(k, P10_GM_TEAM1_INTEL))
                intel_ok += p10_parse_intel(v, &in) && in.state == P10_INTEL_CARRIED;
            if (p10_bytes_equal(k, P10_GM_TEAM2_INTEL))
                intel_ok += p10_parse_intel(v, &in) && in.state == P10_INTEL_DROPPED;
            n++;
        }
        CHECK(n == 8 && intel_ok == 2 && p10_at_end(&r));
        check_malformed_variants("GameModeState ctf", buf, w.size);

        char key[32];
        w = new_writer();
        p10_start_game_mode_state(&w);
        p10_put_map_entry(&w, P10_GM_MODE, "tc");
        p10_put_map_entry(&w, P10_GM_TERRITORY_COUNT, "2");
        snprintf(key, sizeof key, "%s%d", P10_GM_TERRITORY_PREFIX, 0);
        p10_put_map_entry(&w, key, "64,256,40,0");
        snprintf(key, sizeof key, "%s%d", P10_GM_TERRITORY_PREFIX, 1);
        p10_put_map_entry(&w, key, "448,256,40,255");
        p10_put_map_entry(&w, P10_GM_PROGRESS, "1,0,250,-50");
        p10_put_map_end(&w);
        CHECK(p10_begin_game_mode_state(&r, buf, w.size));
        int32_t vals[4];
        int found = 0;
        while (p10_next_map_entry(&r, &k, &v)) {
            if (p10_bytes_equal(k, "territory1"))
                found += p10_parse_ints(v, vals, 4) && vals[3] == P10_GM_TEAM_NEUTRAL;
            if (p10_bytes_equal(k, P10_GM_PROGRESS))
                found += p10_parse_ints(v, vals, 4) && vals[0] == 1 && vals[2] == 250 && vals[3] == -50;
        }
        CHECK(found == 2 && p10_at_end(&r));
        check_malformed_variants("GameModeState tc", buf, w.size);
    }

    /* p10_parse_ints */
    {
        int32_t o[3];
        CHECK(p10_parse_ints(cs("12,-3,40"), o, 3) && o[0] == 12 && o[1] == -3 && o[2] == 40);
        CHECK(p10_parse_ints(cs("2147483647"), o, 1) && o[0] == INT32_MAX);
        CHECK(p10_parse_ints(cs("-2147483648"), o, 1) && o[0] == INT32_MIN);
        CHECK(p10_parse_ints(cs("007"), o, 1) && o[0] == 7);
        const char* bad[] = {"", "1,2", "1,2,3,", "1,2,3,4", " 1,2,3", "1, 2,3", "1,,3", "-,1,2", "1,2,x",
                             "1.5,2,3", "+1,2,3", "2147483648,0,0", "-2147483649,0,0", "99999999999999999999,0,0"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(!p10_parse_ints(cs(bad[i]), o, 3) || (printf("  accepted \"%s\"\n", bad[i]), 0));
        CHECK(!p10_parse_ints(cs("1"), o, 0));
    }

    /* p10_parse_intel / p10_format_intel */
    {
        p10_intel in;
        CHECK(p10_parse_intel(cs("base"), &in) && in.state == P10_INTEL_BASE);
        CHECK(p10_parse_intel(cs("carried"), &in) && in.state == P10_INTEL_CARRIED);
        const char* bad[] = {"", "base:1,2,3", "dropped:1,2,3", "carrier:12", "Base", "home", "carried ", " base"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(!p10_parse_intel(cs(bad[i]), &in) || (printf("  accepted \"%s\"\n", bad[i]), 0));

        p10_intel states[] = {{P10_INTEL_BASE, 0, 0, 0, 0},
                              {P10_INTEL_DROPPED, 0, 0, 0, 0},
                              {P10_INTEL_CARRIED, 0, 0, 0, 0}};
        for (int i = 0; i < 3; i++) {
            char s[64];
            p10_intel back;
            CHECK(p10_format_intel(&states[i], s, sizeof s) && p10_parse_intel(cs(s), &back));
            CHECK(back.state == states[i].state && back.x == states[i].x && back.y == states[i].y &&
                  back.z == states[i].z && back.carrier == states[i].carrier);
        }
        char tiny[4] = "xx";
        CHECK(!p10_format_intel(&states[0], tiny, sizeof tiny) && tiny[0] == '\0');
        p10_intel unknown = {7, 0, 0, 0, 0};
        char s[64];
        CHECK(!p10_format_intel(&unknown, s, sizeof s));
    }
}

static void test_build_and_grenade(void)
{
    {
        p10_build_block in = {{256, 255, 62}, {0x11, 0x22, 0x33}}, out;
        p10_writer w       = new_writer();
        p10_encode_build_block(&w, &in);
        uint8_t want[] = {0x5D, 0x80, 0x04, 0xFE, 0x03, 0x7C, 0x33, 0x22, 0x11};
        check_bytes("BuildBlock", buf, w.size, want, sizeof want);
        CHECK(p10_decode_build_block(buf, w.size, &out) && coord_eq(in.pos, out.pos) && color_eq(in.color, out.color));
        check_malformed_variants("BuildBlock", buf, w.size);

        /* blockcoords are signed and must fit in int32 */
        p10_build_block edge = {{INT32_MIN, INT32_MAX, -1}, {1, 2, 3}};
        w                    = new_writer();
        p10_encode_build_block(&w, &edge);
        CHECK(p10_decode_build_block(buf, w.size, &out) && coord_eq(edge.pos, out.pos) &&
              color_eq(edge.color, out.color));
        w = new_writer();
        p10_write_u8(&w, P10_PKT_BUILD_BLOCK);
        p10_write_svarint(&w, (int64_t) INT32_MAX + 1);
        p10_write_svarint(&w, 0);
        p10_write_svarint(&w, 0);
        p10_write_color(&w, (p10_color) {0, 0, 0});
        CHECK(!p10_decode_build_block(buf, w.size, &out));
    }

    {
        p10_build_line in = {{10, 20, 30}, {10, 20, 25}, {255, 0, 0}}, out;
        p10_writer w      = new_writer();
        p10_encode_build_line(&w, &in);
        uint8_t want[] = {0x5E, 0x14, 0x28, 0x3C, 0x14, 0x28, 0x32, 0x00, 0x00, 0xFF};
        check_bytes("BuildLine", buf, w.size, want, sizeof want);
        CHECK(p10_decode_build_line(buf, w.size, &out) &&
              coord_eq(in.from, out.from) && coord_eq(in.to, out.to) && color_eq(in.color, out.color));
        check_malformed_variants("BuildLine", buf, w.size);
    }

    {
        p10_throw_grenade in = {{1, 2, 3}, {0, 0, -1}, 3.0f}, out;
        p10_writer w         = new_writer();
        p10_encode_throw_grenade(&w, &in);
        uint8_t want[] = {0x5F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x40, 0x40};
        check_bytes("ThrowGrenade", buf, w.size, want, sizeof want);
        CHECK(p10_decode_throw_grenade(buf, w.size, &out) && vec3_eq(out.origin, in.origin) &&
              vec3_eq(out.velocity, in.velocity) && out.fuse == 3.0f);
        check_malformed_variants("ThrowGrenade", buf, w.size);

        /* a fuse of zero explodes at once, which is legal */
        p10_throw_grenade zero = {{1, 2, 3}, {0, 0, 0}, 0.0f};
        w                      = new_writer();
        p10_encode_throw_grenade(&w, &zero);
        CHECK(!w.error && p10_decode_throw_grenade(buf, w.size, &out));

        /* non-finite values and a negative fuse: hand-written, since the
           encoder refuses them */
        p10_throw_grenade bad[] = {
            {{NAN, 2, 3}, {0, 0, 0}, 1.0f},
            {{1, 2, 3}, {0, INFINITY, 0}, 1.0f},
            {{1, 2, 3}, {0, 0, 0}, -INFINITY},
            {{1, 2, 3}, {0, 0, 0}, NAN},
            {{1, 2, 3}, {0, 0, 0}, -0.5f},
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            w = new_writer();
            p10_encode_throw_grenade(&w, &bad[i]);
            CHECK(w.error && w.size == 0);
            w = new_writer();
            p10_write_u8(&w, P10_PKT_THROW_GRENADE);
            p10_write_vec3(&w, bad[i].origin);
            p10_write_vec3(&w, bad[i].velocity);
            p10_write_f32(&w, bad[i].fuse);
            CHECK(!w.error && !p10_decode_throw_grenade(buf, w.size, &out));
        }
    }
}

/* ---- fuzzing: no decoder may crash or read out of bounds (sanitizers) ---- */

static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
    /* xorshift32: deterministic, so a failure reproduces */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz(void)
{
    static const uint8_t types[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 30, 31, 32, 33,
                                    35, 40, 50, 51, 60, 70, 71, 80, 90, 91, 92, 93, 94, 95};
    static uint8_t data[512];
    int accepted = 0;

    /* random payloads behind every valid type */
    for (int i = 0; i < 300000; i++) {
        size_t n = 1 + rng() % (sizeof data - 1);
        data[0]  = types[rng() % sizeof types];
        for (size_t k = 1; k < n; k++)
            data[k] = (uint8_t) rng();
        /* short varints are more interesting than random high bits */
        if (rng() % 2)
            for (size_t k = 1; k < n; k++)
                data[k] &= 0x7f;
        accepted += decode_any(data, n, NULL);
    }

    /* bit flips in a valid, rich packet */
    p10_entity_item player = full_player_item();
    p10_writer w           = new_writer();
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &player);
    p10_put_entity_item(&w, &player);
    size_t n = w.size;
    for (int i = 0; i < 200000; i++) {
        memcpy(data, buf, n);
        int flips = 1 + (int) (rng() % 4);
        for (int f = 0; f < flips; f++)
            data[1 + rng() % (n - 1)] ^= (uint8_t) (1u << (rng() % 8));
        size_t len = rng() % 8 ? n : rng() % (n + 1);
        accepted += decode_any(data, len, NULL);
    }
    printf("fuzz: %d inputs accepted as well formed (no crash is the real check)\n", accepted);
    CHECK(1);
}

static void test_orient(void)
{
    /* front -> euler -> front is the identity for unit vectors */
    float dirs[][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0.6f, 0.8f, 0}, {0, 0.8f, 0.6f},
                       {0.3f, -0.4f, -0.866f}};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        p10_vec3 e = p10_euler_from_front(dirs[i][0], dirs[i][1], dirs[i][2]);
        p10_vec3 f = p10_front_from_euler(e);
        float len  = sqrtf(dirs[i][0] * dirs[i][0] + dirs[i][1] * dirs[i][1] + dirs[i][2] * dirs[i][2]);
        CHECK(fabsf(f.x - dirs[i][0] / len) < 1e-5f && fabsf(f.y - dirs[i][1] / len) < 1e-5f &&
              fabsf(f.z - dirs[i][2] / len) < 1e-5f);
    }
    /* looking down (+z, the map's down) is positive pitch */
    p10_vec3 down = p10_euler_from_front(0, 0.5f, 0.5f);
    CHECK(down.x > 0.7f && down.x < 0.8f);
    /* a zero vector does not produce NaN */
    p10_vec3 zero = p10_euler_from_front(0, 0, 0);
    CHECK(zero.x == 0 && zero.z == 0);
}

/* CP437 <-> UTF-8 at the protocol boundary (text.h) */
static void test_text(void)
{
    char out[300];
    /* "e acute, one half" in CP437 is 82 AB; in UTF-8 C3 A9 C2 BD */
    const uint8_t cp[] = {'h', 'i', 0x82, 0xAB, 0x06};
    size_t n = p10_cp437_to_utf8(cp, sizeof cp, out, sizeof out);
    const uint8_t want[] = {'h', 'i', 0xC3, 0xA9, 0xC2, 0xBD, 0x06};
    check_bytes("cp437 to utf8", (const uint8_t*) out, n, want, sizeof want);
    p10_bytes u = {(const uint8_t*) out, n};
    CHECK(p10_utf8_valid(u));

    char back[16];
    n = p10_utf8_to_cp437(u, back, sizeof back);
    check_bytes("utf8 to cp437", (const uint8_t*) back, n, cp, sizeof cp);

    /* unmappable (euro sign, an emoji) and broken bytes become '?' */
    const uint8_t odd[] = {0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0xFF, 0xC3};
    p10_bytes o = {odd, sizeof odd};
    n = p10_utf8_to_cp437(o, back, sizeof back);
    check_bytes("utf8 unmappable", (const uint8_t*) back, n, (const uint8_t*) "????", 4);

    /* 200 box characters (3 UTF-8 bytes each) are cut to 255 bytes, on a boundary */
    uint8_t box[200];
    memset(box, 0xB0, sizeof box);
    n = p10_cp437_to_utf8(box, sizeof box, out, P10_CHAT_MAX_TEXT + 1);
    CHECK(n == 255);
    u.len = n;
    CHECK(p10_utf8_valid(u));
    memset(box, 0x82, sizeof box); /* 2 bytes each: 127 fit in 254 bytes */
    n = p10_cp437_to_utf8(box, sizeof box, out, P10_CHAT_MAX_TEXT + 1);
    CHECK(n == 254);
    /* stops at NUL, never writes past out_cap */
    n = p10_cp437_to_utf8((const uint8_t*) "ab\0cd", 5, out, 2);
    CHECK(n == 1 && out[0] == 'a' && out[1] == 0);
}

/* the sentry gun, docs/protocol-1.0.md 12.5 */
static void test_sentry(void)
{
    p10_reader r;
    int forced;

    /* golden: EntityUpdate with Create + Health of a sentry owned by player 3 of team 2 */
    {
        p10_entity_item it = {0};
        it.entity_id       = 65792;
        it.mask            = P10_EU_CREATE | P10_EU_HEALTH;
        it.entity_type     = P10_ENT_SENTRY;
        it.owner           = 3;
        it.team            = P10_TEAM_2;
        it.health          = 200;
        p10_writer w       = new_writer();
        p10_start_entity_update(&w, 0);
        p10_put_entity_item(&w, &it);
        uint8_t want[] = {0x1E, 0x00, 0x80, 0x82, 0x04, 0x41, 0x07, 0x03, 0x01, 0xC8};
        check_bytes("Sentry create", buf, w.size, want, sizeof want);
        CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
        p10_entity_item got;
        CHECK(p10_next_entity_item(&r, &got) && got.entity_type == P10_ENT_SENTRY && got.owner == 3 &&
              got.team == P10_TEAM_2 && got.health == 200);
        CHECK(p10_at_end(&r));
        check_malformed_variants("Sentry create", buf, w.size);
    }

    /* an intel (ngspades' Flag) carried by player 9: no owner, team 1, a
       Constant trajectory riding its carrier through parentEntityId */
    {
        p10_entity_item it              = {0};
        it.entity_id                    = 0x7FFF0000u;
        it.mask                         = P10_EU_CREATE | P10_EU_TRAJECTORY;
        it.entity_type                  = P10_ENT_FLAG;
        it.owner                        = P10_NO_OWNER;
        it.team                         = P10_TEAM_1;
        it.trajectory.type              = P10_TRAJ_CONSTANT;
        it.trajectory.has_parent        = 1;
        it.trajectory.parent            = 9;
        p10_writer w                    = new_writer();
        p10_start_entity_update(&w, 0);
        p10_put_entity_item(&w, &it);
        CHECK(!w.error);
        CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
        p10_entity_item got;
        CHECK(p10_next_entity_item(&r, &got) && got.entity_type == P10_ENT_FLAG && got.owner == P10_NO_OWNER &&
              got.team == P10_TEAM_1 && got.trajectory.has_parent && got.trajectory.parent == 9);
        CHECK(p10_at_end(&r));
        check_malformed_variants("Flag create", buf, w.size);
    }

    /* golden: no owner, neutral team */
    {
        p10_entity_item it = {0};
        it.entity_id       = 300;
        it.mask            = P10_EU_CREATE;
        it.entity_type     = P10_ENT_SENTRY;
        it.owner           = P10_NO_OWNER;
        it.team            = P10_TEAM_SPECTATOR;
        p10_writer w       = new_writer();
        p10_put_entity_item(&w, &it);
        uint8_t want[] = {0xAC, 0x02, 0x01, 0x07, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0xFF};
        check_bytes("Sentry create neutral", buf, w.size, want, sizeof want);
    }

    /* golden: EntityEvent Fire, hit */
    {
        p10_entity_event in = {65792, P10_EV_FIRE, 1}, out;
        p10_writer w        = new_writer();
        p10_encode_entity_event(&w, &in);
        uint8_t want[] = {0x1F, 0x80, 0x82, 0x04, 0x04, 0x01};
        check_bytes("EntityEvent fire", buf, w.size, want, sizeof want);
        CHECK(p10_decode_entity_event(buf, w.size, &out) && out.type == P10_EV_FIRE && out.param == 1);
    }

    /* round trip: a full sentry item with a Constant trajectory carrying its aim */
    {
        p10_vec3 e         = {0.3f, 0.0f, -2.0f}; /* looking slightly down, turned */
        p10_entity_item it = {0};
        it.entity_id       = 70000;
        it.mask            = P10_EU_CREATE | P10_EU_FLAGS | P10_EU_TRAJECTORY | P10_EU_HEALTH;
        it.entity_type     = P10_ENT_SENTRY;
        it.owner           = 12;
        it.team            = P10_TEAM_1;
        it.flags           = P10_EF_PLAYER_CLIP | P10_EF_WEAPON_CLIP;
        it.trajectory.type = P10_TRAJ_CONSTANT;
        it.trajectory.origin   = (p10_vec3) {100.5f, 200.5f, 40.0f};
        it.trajectory.rotation = p10_rotation_from_euler(e);
        it.health              = 150;
        p10_entity_item update = {0};
        update.entity_id       = 70000;
        update.mask            = P10_EU_TRAJECTORY;
        update.trajectory      = it.trajectory;

        p10_writer w = new_writer();
        p10_start_entity_update(&w, 0);
        p10_put_entity_item(&w, &it);
        p10_put_entity_item(&w, &update);
        CHECK(!w.error);
        CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
        p10_entity_item got;
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&it, &got) && got.owner == 12 &&
              got.team == P10_TEAM_1);
        CHECK(p10_next_entity_item(&r, &got) && entity_items_equal(&update, &got));
        CHECK(!p10_next_entity_item(&r, &got) && p10_at_end(&r));
        check_malformed_variants("Sentry update", buf, w.size);

        /* the rotation gives back the euler front */
        p10_vec3 f1 = p10_front_from_euler(e);
        p10_vec3 f2 = p10_front_from_rotation(got.trajectory.rotation);
        CHECK(fabsf(f1.x - f2.x) < 1e-4f && fabsf(f1.y - f2.y) < 1e-4f && fabsf(f1.z - f2.z) < 1e-4f);
    }

    /* the identity rotation looks along +y */
    {
        p10_vec3 zero = {0, 0, 0};
        p10_vec3 f    = p10_front_from_rotation(p10_rotation_from_euler(zero));
        CHECK(fabsf(f.x) < 1e-6f && fabsf(f.y - 1.0f) < 1e-6f && fabsf(f.z) < 1e-6f);
    }

    /* a team that is neither 0, 1 nor 255 is malformed */
    {
        uint8_t bad_team[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, P10_EU_CREATE, P10_ENT_SENTRY, 0x03, 0x02};
        CHECK(!decode_any(bad_team, sizeof bad_team, NULL));
        uint8_t no_team[] = {P10_PKT_ENTITY_UPDATE, 0, 0x01, P10_EU_CREATE, P10_ENT_SENTRY, 0x03};
        CHECK(!decode_any(no_team, sizeof no_team, NULL));
    }
}

/* perks, docs/protocol-1.0.md 12.5 */
static void test_perks(void)
{
    p10_reader r;

    /* golden: PlayerUpdate with only the perk bit: 3 points, sentry held, next at 5 */
    {
        p10_player_item it = {0};
        it.player_id       = 4;
        it.mask            = P10_PU_PERKS;
        it.perk_points     = 3;
        it.perk_inventory  = 1 << P10_PERK_SENTRY;
        it.perk_next       = 5;
        p10_writer w       = new_writer();
        p10_start_player_update(&w);
        p10_put_player_item(&w, &it);
        uint8_t want[] = {0x32, 0x04, 0x40, 0x03, 0x02, 0x05};
        check_bytes("PlayerUpdate perks", buf, w.size, want, sizeof want);
        p10_player_item out;
        CHECK(p10_begin_player_update(&r, buf, w.size));
        CHECK(p10_next_player_item(&r, &out) && out.mask == P10_PU_PERKS && out.perk_points == 3 &&
              out.perk_inventory == 2 && out.perk_next == 5);
        CHECK(p10_at_end(&r));
        check_malformed_variants("PlayerUpdate perks", buf, w.size);
        /* an unknown perk in the inventory is malformed (bit 5: past the
           landmine 3 and the dynamite 4) */
        uint8_t bad[] = {0x32, 0x04, 0x40, 0x03, 0x20, 0x05};
        CHECK(!decode_any(bad, sizeof bad, NULL));
    }

    /* round trip with every bit, perks last */
    {
        p10_player_item it = {9, P10_PU_KNOWN, cs("Deuce"), P10_PF_ADMIN, 7, P10_TEAM_1, 7, 1, 20, 6, 3, 0};
        p10_writer w       = new_writer();
        p10_start_player_update(&w);
        p10_put_player_item(&w, &it);
        p10_player_item out;
        CHECK(p10_begin_player_update(&r, buf, w.size));
        CHECK(p10_next_player_item(&r, &out) && out.ping_ms == 20 && out.perk_points == 6 && out.perk_inventory == 3 &&
              out.perk_next == 0 && out.kills == 7 && p10_bytes_equal(out.name, "Deuce"));
        CHECK(p10_at_end(&r));
    }

    /* golden: PlayerAction use the sentry perk on the floor block 300,200,40 */
    {
        uint64_t param = p10_perk_param(P10_PERK_SENTRY, 300, 200, 40);
        CHECK(param == (1ull | (300ull << 8) | (200ull << 20) | (40ull << 32)));
        p10_player_action in = {P10_EV_USE_PERK, param}, out;
        p10_writer w         = new_writer();
        p10_encode_player_action(&w, &in);
        /* type 60, event 5, then param as a varint */
        uint8_t want[] = {0x3C, 0x05, 0x81, 0xD8, 0x84, 0xE4, 0x80, 0x05};
        check_bytes("PlayerAction use perk", buf, w.size, want, sizeof want);
        CHECK(p10_decode_player_action(buf, w.size, &out) && out.type == P10_EV_USE_PERK && out.param == param);
        uint8_t  perk;
        uint32_t x, y, z;
        CHECK(p10_perk_param_decode(out.param, &perk, &x, &y, &z) && perk == P10_PERK_SENTRY && x == 300 &&
              y == 200 && z == 40);
        CHECK(!p10_perk_param_decode(1ull << 40, &perk, &x, &y, &z));
        /* the jetpack carries no cell */
        CHECK(p10_perk_param(P10_PERK_JETPACK, 0, 0, 0) == 0);
    }
}

/* revision 1.2's compact Player trajectory: within its steps of the floats,
   half the bytes, read back as P10_TRAJ_PLAYER; a value it cannot hold
   falls back to the float form, read back exactly */
static void test_compact(void)
{
    p10_reader      r;
    int             forced;
    p10_entity_item it = {0}, back;
    it.entity_id       = 7;
    it.mask            = P10_EU_TRAJECTORY;
    it.trajectory.type = P10_TRAJ_PLAYER;
    it.trajectory.origin   = (p10_vec3) {300.3f, 12.7f, -1.3f};
    it.trajectory.velocity = (p10_vec3) {0.21f, -0.07f, 1.9f};
    it.trajectory.euler    = (p10_vec3) {-0.6f, 0.0f, 2.9f};

    p10_writer w = new_writer();
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    size_t full = w.size;

    it.trajectory.type = P10_TRAJ_PLAYER_COMPACT;
    w                  = new_writer();
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    CHECK(!w.error);
    CHECK(w.size + 20 == full); /* 17 trajectory bytes instead of 37 */
    CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
    CHECK(p10_next_entity_item(&r, &back) && p10_at_end(&r));
    const p10_trajectory* t = &back.trajectory;
    CHECK(t->type == P10_TRAJ_PLAYER);
    CHECK(fabsf(t->origin.x - 300.3f) <= 0.5f / 64 && fabsf(t->origin.y - 12.7f) <= 0.5f / 64 &&
          t->origin.z <= -1.3f && t->origin.z > -1.3f - 1.0f / 64);
    CHECK(fabsf(t->velocity.x - 0.21f) <= 0.5f / 1024 && fabsf(t->velocity.z - 1.9f) <= 0.5f / 1024);
    CHECK(fabsf(t->euler.x + 0.6f) < 1e-4f && fabsf(t->euler.z - 2.9f) < 1e-4f && t->euler.y == 0.0f);

    /* a body 0.001 over a block's top (feet at z 49, map z grows down) is
       never read back on it or under it: the client's step-up took that
       floor for a step and the body bobbed */
    it.trajectory.origin.z = 46.749f;
    w                      = new_writer();
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
    CHECK(p10_next_entity_item(&r, &back) && p10_at_end(&r));
    CHECK(back.trajectory.origin.z + 2.25f < 49.0f && back.trajectory.origin.z > 46.749f - 1.0f / 64);

    /* off the compact range (a map past 1024): the floats, exactly */
    it.trajectory.origin.x = 1500.25f;
    w                      = new_writer();
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    CHECK(w.size == full);
    CHECK(p10_begin_entity_update(&r, buf, w.size, &forced));
    CHECK(p10_next_entity_item(&r, &back) && p10_at_end(&r));
    CHECK(back.trajectory.type == P10_TRAJ_PLAYER && back.trajectory.origin.x == 1500.25f &&
          back.trajectory.velocity.z == 1.9f);
}

int main(void)
{
    test_orient();
    test_compact();
    test_golden();
    test_primitives();
    test_session();
    test_game_state();
    test_entities();
    test_inventory();
    test_players();
    test_events();
    test_terrain();
    test_hits();
    test_chat();
    test_game_mode_state();
    test_build_and_grenade();
    test_fuzz();
    test_text();
    test_sentry();
    test_perks();
    printf("%s: %d checks, %d failure(s)\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures != 0;
}
