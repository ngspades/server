// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Packets/Packets.h>
#include <Util/Alloc.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <mersenne-twister/MT.h>
#include <ctype.h>
#include <Server/MapStore.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Server/MapMeta/Internal.h>


/*
 * Reading a pyspades map script without running Python.
 *
 * The file is cut into logical lines (comments dropped, bracketed and
 * backslash continuations joined, "if c: stmt" split in two), the top-level
 * assignments are evaluated as Python literals with a little arithmetic
 * (Nine-Eye_Babel computes its spawn rectangles from AREA and WIDTH), and
 * the functions pyspades calls are matched against the idioms the maps use.
 * Names the evaluator does not know become opaque objects, so
 * `team.protocol.map.get_z(x, y)`, `random.choice(list)` and
 * `connection.protocol.get_random_location(True, rect)` can be recognised
 * by their last attribute. Anything else fails quietly and the function is
 * then reported as not applied.
 */

mm_meta_t mm_meta;

/* ------------------------------------------------------------------ */
/* loading                                                              */
/* ------------------------------------------------------------------ */

static void _append(char* buf, size_t size, const char* what)
{
    size_t len = strlen(buf);
    snprintf(buf + len, size - len, "%s%s", len ? ", " : "", what);
}

static void _copy_str(char* dst, size_t size, const char* key)
{
    val_t* v = mm_env_get(mm_globals, key);
    if (v != NULL && v->t == V_STR) {
        snprintf(dst, size, "%s", v->s);
    } else if (v != NULL && v->t == V_NUM) {
        snprintf(dst, size, "%g", v->num);
    }
}

static int _ext_bool(val_t* v)
{
    return (v->t == V_BOOL || v->t == V_NUM) && v->num != 0;
}

static void _read_extensions(val_t* ext, char* not_applied, size_t size, char* informational, size_t info_size)
{
    for (int k = 0; k < ext->n; k++) {
        if (ext->keys[k]->t != V_STR) {
            continue;
        }
        const char* key = ext->keys[k]->s;
        val_t*      v   = ext->items[k];
        double      n[4];
        if (!strcmp(key, "water_damage") && v->t == V_NUM && v->num >= 0) {
            mm_meta.has_water    = 1;
            mm_meta.water_damage = v->num > 255 ? 255 : (int) v->num;
        } else if (!strcmp(key, "boundary_damage") && v->t == V_DICT) {
            static const char* const sides[] = {"left", "right", "top", "bottom", "damage"};
            double                   side[5];
            int                      ok = 1;
            for (int s = 0; s < 5; s++) {
                val_t* found = NULL;
                for (int j = 0; j < v->n; j++) {
                    if (v->keys[j]->t == V_STR && !strcmp(v->keys[j]->s, sides[s])) {
                        found = v->items[j];
                    }
                }
                ok &= found != NULL && found->t == V_NUM;
                side[s] = ok ? found->num : 0;
            }
            if (ok && side[0] < side[1] && side[2] < side[3] && side[4] > 0) {
                mm_meta.has_boundary = 1;
                for (int s = 0; s < 4; s++) {
                    mm_meta.bound[s] = (float) side[s];
                }
                mm_meta.boundary_damage = side[4] > 255 ? 255 : (int) side[4];
            } else {
                _append(not_applied, size, key);
            }
        } else if ((!strcmp(key, "arena_blue_spawn") || !strcmp(key, "arena_green_spawn")) &&
                   mm_numbers(v, 3, n, 3) == 3)
        {
            int team = key[6] == 'b' ? 0 : 1;
            /* the arena spawn is the team's spawn when the script has none */
            if (mm_meta.point_count[team] == 0) {
                mm_add_point(team, n);
            }
        } else if ((!strcmp(key, "arena_blue_spawns") || !strcmp(key, "arena_green_spawns")) && v->t == V_SEQ) {
            int team = key[6] == 'b' ? 0 : 1;
            /* every tuple of the list: the script picks one at random for
               each spawn (arena_blue_spawns = ((x, y, z), ...)), and reading
               only the first put the whole team on one cell */
            if (mm_meta.point_count[team] == 0) {
                for (int j = 0; j < v->n; j++) {
                    if (mm_numbers(v->items[j], 3, n, 3) == 3) {
                        mm_add_point(team, n);
                    }
                }
            }
        } else if (!strcmp(key, "arena_spawn_change") && v->t == V_NUM && v->num >= 0 && v->num <= 4) {
            mm_meta.arena_spawn_change = (int) v->num;
        } else if (!strcmp(key, "arena_max_spawn_distance") && v->t == V_NUM && v->num > 0) {
            mm_meta.arena_max_spawn_distance = (float) v->num;
        } else if (!strcmp(key, "arena_gates") && v->t == V_SEQ) {
            for (int j = 0; j < v->n && mm_meta.gate_count < MM_MAX_GATES; j++) {
                if (mm_numbers(v->items[j], 3, n, 3) == 3) {
                    for (int c = 0; c < 3; c++) {
                        mm_meta.gates[mm_meta.gate_count][c] = (int) n[c];
                    }
                    mm_meta.gate_count++;
                }
            }
        } else if (!strcmp(key, "heavens_color") && mm_numbers(v, 3, n, 4) >= 3) {
            mm_meta.has_heavens = 1;
            mm_meta.heavens     = 0xFF000000u | ((uint32_t) n[0] & 0xFF) << 16 | ((uint32_t) n[1] & 0xFF) << 8 |
                           ((uint32_t) n[2] & 0xFF);
        } else if ((!strcmp(key, "arena") || !strcmp(key, "babel")) && v->t == V_BOOL) {
            /* the mode is chosen by --gamemode and the catalog: noted only */
            if (_ext_bool(v)) {
                _append(informational, info_size, key);
            }
        } else {
            _append(not_applied, size, key);
        }
    }
}

static void _reset(void)
{
    memset(&mm_meta, 0, sizeof(mm_meta));
    /* 0 is a real spawn change rule (no change), so "the map did not say" is
       its own value */
    mm_meta.arena_spawn_change = -1;
    mm_lines      = NULL;
    mm_line_count = 0;
    mm_line_cap   = 0;
    mm_globals    = NULL;
}

void map_meta_load(server_t* server, const char* path)
{
    _reset();
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        LOG_INFO("No map metadata at %s: spawns, intel and tents come from the map toml", path);
        evt_log(NULL, "map_meta", "file=none");
        return;
    }
    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    fseek(file, 0L, SEEK_SET);
    if (size < 0 || size > 1024 * 1024) {
        fclose(file);
        LOG_WARNING("Map metadata %s: unreadable size, ignored", path);
        return;
    }
    char*  src  = mm_alloc((size_t) size + 1);
    size_t read = fread(src, 1, (size_t) size, file);
    fclose(file);
    src[read] = '\0';

    /* a script saved with a UTF-8 byte order mark (Battlecity's): its first
       statement read as the three bytes and was reported not applied */
    if (read >= 3 && (unsigned char) src[0] == 0xEF && (unsigned char) src[1] == 0xBB &&
        (unsigned char) src[2] == 0xBF)
    {
        memmove(src, src + 3, read - 3 + 1);
        read -= 3;
    }

    mm_split_lines(src, read);

    /* pass 1: the top-level assignments, in order, as Python runs them */
    char not_applied[512] = "";
    char informational[64] = "";
    for (int i = 0; i < mm_line_count; i++) {
        const char* text = mm_lines[i].text;
        if (mm_lines[i].indent > 0 || mm_starts_kw(text, "import") || mm_starts_kw(text, "from") ||
            mm_starts_kw(text, "global") || mm_starts_kw(text, "pass") || mm_starts_kw(text, "def"))
        {
            continue;
        }
        if (mm_is_compound(text)) {
            char what[48];
            snprintf(what, sizeof(what), "top-level %.20s", text);
            _append(not_applied, sizeof(not_applied), what);
            continue;
        }
        int eq = mm_assign_pos(text);
        if (eq < 0) {
            char what[48];
            snprintf(what, sizeof(what), "statement '%.20s'", text);
            _append(not_applied, sizeof(not_applied), what);
            continue;
        }
        size_t lhs = (size_t) eq;
        while (lhs > 0 && text[lhs - 1] == ' ') {
            lhs--;
        }
        val_t* v = mm_eval(server, NULL, text + eq + 1);
        if (!mm_is_identifier(text, lhs)) {
            continue;
        }
        char* name = mm_strndup(text, lhs);
        if (v->t == V_ERR) {
            char what[48];
            snprintf(what, sizeof(what), "unreadable %.30s", name);
            _append(not_applied, sizeof(not_applied), what);
            continue;
        }
        mm_env_set(&mm_globals, name, v);
    }

    _copy_str(mm_meta.name, sizeof(mm_meta.name), "name");
    _copy_str(mm_meta.version, sizeof(mm_meta.version), "version");
    _copy_str(mm_meta.author, sizeof(mm_meta.author), "author");
    _copy_str(mm_meta.description, sizeof(mm_meta.description), "description");

    double n[3];
    val_t* v = mm_env_get(mm_globals, "fog");
    if (v != NULL && mm_numbers(v, 3, n, 3) == 3) {
        mm_meta.has_fog = 1;
        for (int c = 0; c < 3; c++) {
            mm_meta.fog[c] = (uint8_t) n[c];
        }
    }
    v = mm_env_get(mm_globals, "protected");
    for (int k = 0; v != NULL && v->t == V_SEQ && k < v->n; k++) {
        const val_t* s = v->items[k];
        /* pyspades coordinates(): the letter is the x sector, the digit y */
        if (s->t == V_STR && strlen(s->s) == 2 && toupper((unsigned char) s->s[0]) >= 'A' &&
            toupper((unsigned char) s->s[0]) <= 'H' && s->s[1] >= '1' && s->s[1] <= '8' &&
            mm_meta.sector_count < MM_MAX_SECTORS)
        {
            mm_meta.sectors[mm_meta.sector_count][0] = (toupper((unsigned char) s->s[0]) - 'A') * MM_SECTOR;
            mm_meta.sectors[mm_meta.sector_count][1] = (s->s[1] - '1') * MM_SECTOR;
            mm_meta.sector_count++;
        } else {
            _append(not_applied, sizeof(not_applied), "protected entry");
        }
    }

    /* pass 2: the functions pyspades calls. Spawns before the extensions:
       an arena spawn only fills a team the script left empty */
    for (int i = 0; i < mm_line_count; i++) {
        if (mm_lines[i].indent > 0 || !mm_starts_kw(mm_lines[i].text, "def")) {
            continue;
        }
        mm_func_t f   = {0};
        const char* s = mm_lines[i].text + 3;
        while (*s == ' ') {
            s++;
        }
        const char* paren = strchr(s, '(');
        if (paren == NULL) {
            continue;
        }
        size_t name_len = (size_t) (paren - s);
        while (name_len > 0 && s[name_len - 1] == ' ') {
            name_len--;
        }
        f.name = mm_strndup(s, name_len);
        /* parameter names, defaults dropped */
        const char* p = paren + 1;
        while (*p && *p != ')' && f.nparams < 8) {
            while (*p == ' ' || *p == '*') {
                p++;
            }
            const char* start = p;
            while (*p && (isalnum((unsigned char) *p) || *p == '_')) {
                p++;
            }
            if (p > start) {
                f.params[f.nparams++] = mm_strndup(start, (size_t) (p - start));
            }
            while (*p && *p != ',' && *p != ')') {
                p++;
            }
            if (*p == ',') {
                p++;
            }
        }
        int j = i + 1;
        while (j < mm_line_count && mm_lines[j].indent > 0) {
            j++;
        }
        f.body  = mm_lines + i + 1;
        f.count = j - i - 1;

        int ok;
        if (!strcmp(f.name, "get_entity_location")) {
            ok = mm_do_entity_location(server, &f);
        } else if (!strcmp(f.name, "get_spawn_location")) {
            ok = mm_do_spawn_location(server, &f);
        } else if (!strcmp(f.name, "on_block_destroy") || !strcmp(f.name, "on_block_build_attempt")) {
            ok = mm_do_protection(server, &f, "False");
        } else if (!strcmp(f.name, "is_indestructable")) {
            ok = mm_do_protection(server, &f, "True");
        } else {
            ok = 0;
        }
        if (!ok) {
            _append(not_applied, sizeof(not_applied), f.name);
        }
    }

    v = mm_env_get(mm_globals, "extensions");
    if (v != NULL && v->t == V_DICT) {
        _read_extensions(v, not_applied, sizeof(not_applied), informational, sizeof(informational));
    }

    mm_meta.loaded = 1;
    LOG_STATUS("Map metadata: '%s' version %s by %s%s%s",
               mm_meta.name,
               mm_meta.version[0] ? mm_meta.version : "?",
               mm_meta.author[0] ? mm_meta.author : "?",
               mm_meta.description[0] ? ": " : "",
               mm_meta.description);
    if (not_applied[0]) {
        LOG_WARNING("map script rules not applied: %s", not_applied);
    }
    evt_log(NULL,
            "map_meta",
            "file=%s entities=%d%d%d%d spawns=%d,%d rects=%d,%d sectors=%d protect_rules=%d boundary=%s gates=%d "
            "water=%d fog=%d heavens=%08x arena_change=%d arena_distance=%g made_for=%s not_applied=\"%s\"",
            path,
            mm_meta.has_entity[0][0],
            mm_meta.has_entity[0][1],
            mm_meta.has_entity[1][0],
            mm_meta.has_entity[1][1],
            mm_meta.point_count[0],
            mm_meta.point_count[1],
            mm_meta.has_rect[0],
            mm_meta.has_rect[1],
            mm_meta.sector_count,
            mm_meta.rules.n,
            mm_meta.has_boundary ? "yes" : "no",
            mm_meta.gate_count,
            mm_meta.has_water ? mm_meta.water_damage : -1,
            mm_meta.has_fog,
            (unsigned) mm_meta.heavens,
            mm_meta.arena_spawn_change,
            mm_meta.arena_max_spawn_distance,
            informational[0] ? informational : "-",
            not_applied);
    if (mm_meta.has_boundary) {
        evt_log(NULL,
                "map_meta",
                "boundary left=%g right=%g top=%g bottom=%g damage=%d",
                mm_meta.bound[0],
                mm_meta.bound[1],
                mm_meta.bound[2],
                mm_meta.bound[3],
                mm_meta.boundary_damage);
    }
    for (int k = 0; k < mm_meta.rules.n; k++) {
        static const char* const op_names[] = {"==", "!=", "<", "<=", ">", ">="};
        char                     text[128]  = "";
        for (int a = 0; a < mm_meta.rules.g[k].n; a++) {
            size_t len = strlen(text);
            snprintf(text + len, sizeof(text) - len, "%s%c%s%g", a ? "&" : "", "xyz"[mm_meta.rules.g[k].a[a].axis],
                     op_names[mm_meta.rules.g[k].a[a].op], mm_meta.rules.g[k].a[a].v);
        }
        evt_log(NULL, "map_meta", "protect rule=%s", mm_meta.rules.g[k].n ? text : "all");
    }
    for (int k = 0; k < mm_meta.sector_count; k++) {
        evt_log(NULL, "map_meta", "protect sector=%d,%d-%d,%d", mm_meta.sectors[k][0], mm_meta.sectors[k][1],
                mm_meta.sectors[k][0] + MM_SECTOR - 1, mm_meta.sectors[k][1] + MM_SECTOR - 1);
    }
    mm_pool_free();
    mm_lines   = NULL;
    mm_globals = NULL;
}

void map_meta_apply(server_t* server)
{
    if (!mm_meta.loaded) {
        return;
    }
    if (mm_meta.has_fog) {
        server->protocol.color_fog.r = mm_meta.fog[0];
        server->protocol.color_fog.g = mm_meta.fog[1];
        server->protocol.color_fog.b = mm_meta.fog[2];
    }
    if (mm_meta.has_water) {
        server->protocol.gamemode.water_damage_enabled = mm_meta.water_damage > 0;
        server->protocol.gamemode.water_damage         = (uint8_t) mm_meta.water_damage;
    }
    /* the spawn area every other part of the server reads (the patrol
       anchor, the random intel fallback): the author's rectangle, or the box
       around the author's points */
    for (int t = 0; t < 2; t++) {
        quad3d_t* q = &server->protocol.spawns[t];
        if (mm_meta.has_rect[t]) {
            q->from = (vector3f_t) {fminf(mm_meta.rect[t][0], mm_meta.rect[t][2]), fminf(mm_meta.rect[t][1], mm_meta.rect[t][3]), 0};
            q->to   = (vector3f_t) {fmaxf(mm_meta.rect[t][0], mm_meta.rect[t][2]), fmaxf(mm_meta.rect[t][1], mm_meta.rect[t][3]), 0};
        } else if (mm_meta.point_count[t] > 0) {
            q->from = q->to = mm_meta.points[t][0];
            for (int k = 1; k < mm_meta.point_count[t]; k++) {
                vector3f_t pt = mm_meta.points[t][k];
                q->from       = (vector3f_t) {fminf(q->from.x, pt.x), fminf(q->from.y, pt.y), fminf(q->from.z, pt.z)};
                q->to         = (vector3f_t) {fmaxf(q->to.x, pt.x), fmaxf(q->to.y, pt.y), fmaxf(q->to.z, pt.z)};
            }
        } else {
            continue;
        }
        LOG_INFO("Team %d spawn area from the map metadata: %.0f,%.0f - %.0f,%.0f", t + 1, q->from.x, q->from.y,
                 q->to.x, q->to.y);
    }
}

int map_meta_entity(server_t* server, int team, int kind, vector3f_t* out)
{
    if (team < 0 || team > 1 || kind < 0 || kind > 1 || !mm_meta.has_entity[team][kind]) {
        return 0;
    }
    map_store_t* map = &server->s_map.map;
    int       x   = (int) mm_meta.entity[team][kind].x;
    int       y   = (int) mm_meta.entity[team][kind].y;
    int       z   = (int) mm_meta.entity[team][kind].z;
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    z = z < 0 ? 0 : z > map->size_z - 1 ? map->size_z - 1 : z;
    /* pyspades' update_entities: an entity rests on the first solid block
       at or below it, and climbs out of a block built over it */
    if (z > 0 && map_solid(map, x, y, z - 1)) {
        while (z > 0 && map_solid(map, x, y, z - 1)) {
            z--;
        }
    } else {
        while (z < map->size_z - 1 && !map_solid(map, x, y, z)) {
            z++;
        }
    }
    *out = (vector3f_t) {(float) x, (float) y, (float) z};
    return 1;
}

int map_meta_spawn(server_t* server, int team, vector3f_t* out)
{
    if (team < 0 || team > 1 || mm_meta.point_count[team] == 0) {
        return 0;
    }
    int index = (int) (gen_rand_long(&server->rand) % (unsigned long) mm_meta.point_count[team]);
    return map_meta_spawn_from(server, team, index, out);
}

int map_meta_spawn_count(server_t* server, int team)
{
    (void) server;
    return team >= 0 && team <= 1 ? mm_meta.point_count[team] : 0;
}

int map_meta_spawn_from(server_t* server, int team, int index, vector3f_t* out)
{
    (void) server;
    if (team < 0 || team > 1 || index < 0 || index >= mm_meta.point_count[team]) {
        return 0;
    }
    vector3f_t pt = mm_meta.points[team][index];
    *out          = (vector3f_t) {floorf(pt.x) + 0.5f, floorf(pt.y) + 0.5f, pt.z};
    return 1;
}

int map_meta_arena_spawn_change(server_t* server)
{
    (void) server;
    return mm_meta.arena_spawn_change;
}

float map_meta_arena_max_spawn_distance(server_t* server)
{
    (void) server;
    return mm_meta.arena_max_spawn_distance;
}

static int _atom_holds(const mm_atom_t* a, float v)
{
    switch (a->op) {
        case OP_EQ:
            return v == a->v;
        case OP_NE:
            return v != a->v;
        case OP_LT:
            return v < a->v;
        case OP_LE:
            return v <= a->v;
        case OP_GT:
            return v > a->v;
        case OP_GE:
            return v >= a->v;
    }
    return 0;
}

int map_meta_cell_protected(server_t* server, int x, int y, int z)
{
    (void) server;
    for (int k = 0; k < mm_meta.sector_count; k++) {
        if (x >= mm_meta.sectors[k][0] && x < mm_meta.sectors[k][0] + MM_SECTOR && y >= mm_meta.sectors[k][1] &&
            y < mm_meta.sectors[k][1] + MM_SECTOR)
        {
            return 1;
        }
    }
    float xyz[3] = {(float) x, (float) y, (float) z};
    for (int k = 0; k < mm_meta.rules.n; k++) {
        int holds = 1;
        for (int a = 0; a < mm_meta.rules.g[k].n && holds; a++) {
            holds = _atom_holds(&mm_meta.rules.g[k].a[a], xyz[mm_meta.rules.g[k].a[a].axis]);
        }
        if (holds) {
            return 1;
        }
    }
    return 0;
}

int map_meta_play_area(server_t* server, int* x0, int* y0, int* x1, int* y1)
{
    if (!mm_meta.has_boundary) {
        *x0 = 0;
        *y0 = 0;
        *x1 = server->s_map.map.size_x - 1;
        *y1 = server->s_map.map.size_y - 1;
        return 0;
    }
    /* pyspades hurts at x <= left: the first whole safe cell is left + 1 */
    *x0 = (int) floorf(mm_meta.bound[0]) + 1;
    *y0 = (int) floorf(mm_meta.bound[2]) + 1;
    *x1 = (int) ceilf(mm_meta.bound[1]) - 1;
    *y1 = (int) ceilf(mm_meta.bound[3]) - 1;
    return 1;
}

int map_meta_arena_gates(server_t* server, int (*seeds)[3], int max)
{
    (void) server;
    int n = mm_meta.gate_count < max ? mm_meta.gate_count : max;
    for (int k = 0; k < n; k++) {
        memcpy(seeds[k], mm_meta.gates[k], sizeof(mm_meta.gates[k]));
    }
    return n;
}

uint32_t map_meta_babel_color(server_t* server, uint32_t fallback)
{
    (void) server;
    return mm_meta.has_heavens ? mm_meta.heavens : fallback;
}

void map_meta_player_tick(server_t* server, player_t* player)
{
    if (!mm_meta.has_boundary || player->state != STATE_READY || !player->alive) {
        return;
    }
    /* map_extensions.py: outside the rectangle, damage on every position
       update, which a 0.75 client sends once a second */
    float x = player->movement.position.x;
    float y = player->movement.position.y;
    if (x <= mm_meta.bound[0] || x >= mm_meta.bound[1] || y <= mm_meta.bound[2] || y >= mm_meta.bound[3]) {
        evt_log(player,
                "boundary_damage",
                "pos=%.1f,%.1f damage=%d hp=%d area=%g,%g-%g,%g",
                x,
                y,
                mm_meta.boundary_damage,
                (int) player->hp,
                mm_meta.bound[0],
                mm_meta.bound[2],
                mm_meta.bound[1],
                mm_meta.bound[3]);
        vector3f_t zero = {0, 0, 0};
        send_set_hp(server, player, player, mm_meta.boundary_damage, 0, 4, 5, 0, zero);
    }
}
