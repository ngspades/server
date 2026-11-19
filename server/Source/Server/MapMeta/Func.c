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


/* ------------------------------------------------------------------ */
/* the script's functions                                               */
/* ------------------------------------------------------------------ */


/* "return expr" -> expr; NULL for other statements */
static const char* _after_kw(const char* text, const char* kw)
{
    if (!mm_starts_kw(text, kw)) {
        return NULL;
    }
    text += strlen(kw);
    while (*text == ' ') {
        text++;
    }
    return text;
}

/* "name = expr": the '=' that is an assignment, -1 if none */
int mm_assign_pos(const char* s)
{
    size_t n     = strlen(s);
    int    depth = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            i = mm_skip_string(s, i, n) - 1;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            depth--;
        } else if (c == '=' && depth == 0) {
            char before = i > 0 ? s[i - 1] : ' ';
            char after  = s[i + 1];
            if (after == '=' || strchr("=!<>+-*/%&|^", before) != NULL) {
                if (after == '=') {
                    i++;
                }
                continue;
            }
            return (int) i;
        }
    }
    return -1;
}

int mm_is_identifier(const char* s, size_t n)
{
    if (n == 0 || !(isalpha((unsigned char) s[0]) || s[0] == '_')) {
        return 0;
    }
    for (size_t k = 1; k < n; k++) {
        if (!(isalnum((unsigned char) s[k]) || s[k] == '_')) {
            return 0;
        }
    }
    return 1;
}

/* the condition of "if c:" / "elif c:", NULL otherwise */
static char* _if_cond(const char* text)
{
    const char* rest = _after_kw(text, "if");
    if (rest == NULL) {
        rest = _after_kw(text, "elif");
    }
    if (rest == NULL) {
        return NULL;
    }
    size_t len = strlen(rest);
    if (len > 0 && rest[len - 1] == ':') {
        len--;
    }
    return mm_strndup(rest, len);
}

/* a local assignment, "x = expr" (other targets are evaluated, not kept) */
static val_t* _local_statement(server_t* server, env_t** locals, const char* text, uint8_t* is_return)
{
    const char* expr = _after_kw(text, "return");
    *is_return       = expr != NULL;
    if (expr != NULL) {
        return *expr ? mm_eval(server, *locals, expr) : &mm_err_value;
    }
    int eq = mm_assign_pos(text);
    if (eq < 0) {
        return &mm_err_value;
    }
    size_t lhs = (size_t) eq;
    while (lhs > 0 && text[lhs - 1] == ' ') {
        lhs--;
    }
    val_t* v = mm_eval(server, *locals, text + eq + 1);
    if (v->t != V_ERR && mm_is_identifier(text, lhs)) {
        mm_env_set(locals, mm_strndup(text, lhs), v);
    }
    return v;
}

static const char* const entity_names[2][2] = {{"BLUE_FLAG", "BLUE_BASE"}, {"GREEN_FLAG", "GREEN_BASE"}};

static int _entity_in(const char* text, int* team, int* kind)
{
    for (int t = 0; t < 2; t++) {
        for (int k = 0; k < 2; k++) {
            const char* at = strstr(text, entity_names[t][k]);
            if (at != NULL && !(isalnum((unsigned char) at[strlen(entity_names[t][k])]) ||
                                at[strlen(entity_names[t][k])] == '_'))
            {
                *team = t;
                *kind = k;
                return 1;
            }
        }
    }
    return 0;
}

/* if entity_id == BLUE_FLAG: return (x, y, z) */
int mm_do_entity_location(server_t* server, const mm_func_t* f)
{
    env_t* locals = NULL;
    int    team = -1, kind = -1, found = 0, failed = 0;
    for (int i = 0; i < f->count; i++) {
        const char* text = f->body[i].text;
        if (mm_starts_kw(text, "if") || mm_starts_kw(text, "elif")) {
            if (!_entity_in(text, &team, &kind)) {
                team = kind = -1;
                failed      = 1;
            }
            continue;
        }
        if (mm_starts_kw(text, "else")) {
            team = kind = -1;
            continue;
        }
        uint8_t is_return;
        val_t*  v = _local_statement(server, &locals, text, &is_return);
        if (!is_return) {
            continue;
        }
        double xyz[3];
        if (team < 0 || mm_numbers(v, 3, xyz, 3) != 3) {
            failed = 1;
            continue;
        }
        mm_meta.has_entity[team][kind] = 1;
        mm_meta.entity[team][kind]     = (vector3f_t) {(float) xyz[0], (float) xyz[1], (float) xyz[2]};
        found++;
    }
    return found > 0 && !failed;
}

void mm_add_point(int team, const double xyz[3])
{
    if (mm_meta.point_count[team] < MM_MAX_POINTS) {
        mm_meta.points[team][mm_meta.point_count[team]++] =
        (vector3f_t) {(float) xyz[0], (float) xyz[1], (float) xyz[2]};
    }
}

static int _team_in(const char* text)
{
    return strstr(text, "blue_team") ? 0 : strstr(text, "green_team") ? 1 : -1;
}

/* per team: random.choice(points), a constant return, or
   get_random_location(True, rect) */
int mm_do_spawn_location(server_t* server, const mm_func_t* f)
{
    env_t* locals      = NULL;
    int    team        = -1;
    int    team_indent = 0;
    int    got[2]      = {0, 0};
    for (int i = 0; i < f->count; i++) {
        const char* text   = f->body[i].text;
        int         indent = f->body[i].indent;
        int         is_if  = mm_starts_kw(text, "if") || mm_starts_kw(text, "elif") || mm_starts_kw(text, "else");
        if (team >= 0 && indent <= team_indent) {
            team = -1;
        }
        if (is_if) {
            int t = _team_in(text);
            if (t >= 0) {
                team        = t;
                team_indent = indent;
            }
            continue;
        }
        uint8_t is_return;
        val_t*  v = _local_statement(server, &locals, text, &is_return);
        if (team < 0) {
            continue;
        }
        double xyz[4];
        if (v->t == V_CHOICE) {
            val_t* list = v->items[0];
            for (int k = 0; k < list->n; k++) {
                /* (x, y) points too: "x, y = choice(points)" and then
                   "z = map.get_z(x, y)" (Beaulieu_Abbey_Zombie), the
                   ground's top like get_z */
                int n = mm_numbers(list->items[k], 2, xyz, 3);
                if (n == 2 && (xyz[0] < 0 || xyz[1] < 0 || xyz[0] >= server->s_map.map.size_x ||
                               xyz[1] >= server->s_map.map.size_y))
                {
                    continue;
                }
                if (n == 2) {
                    xyz[2] = map_top(&server->s_map.map, (uint16_t) xyz[0], (uint16_t) xyz[1]);
                }
                if (n >= 2) {
                    mm_add_point(team, xyz);
                    got[team] = 1;
                }
            }
        } else if (v->t == V_RANDLOC && mm_numbers(v->items[0], 4, xyz, 4) == 4) {
            mm_meta.has_rect[team] = 1;
            for (int k = 0; k < 4; k++) {
                mm_meta.rect[team][k] = (float) xyz[k];
            }
            got[team] = 1;
        } else if (is_return && mm_numbers(v, 3, xyz, 3) == 3) {
            mm_add_point(team, xyz);
            got[team] = 1;
        }
    }
    return got[0] && got[1];
}

/* on_block_destroy / on_block_build_attempt return False, is_indestructable
   returns True, when the cell is protected: "if <condition on x, y, z>:" */
int mm_do_protection(server_t* server, const mm_func_t* f, const char* deny)
{
    if (f->nparams < 4) {
        return 0;
    }
    const char* coord[3] = {f->params[1], f->params[2], f->params[3]};
    mm_dnf_t    cond     = {0};
    int         cond_ok = 0, cond_indent = 0, failed = 0, rules = 0;
    int         base     = f->count > 0 ? f->body[0].indent : 0;
    for (int i = 0; i < f->count; i++) {
        const char* text   = f->body[i].text;
        int         indent = f->body[i].indent;
        char*       c      = _if_cond(text);
        if (c != NULL) {
            cond_ok     = mm_parse_cond(server, c, coord, &cond);
            cond_indent = indent;
            failed |= !cond_ok;
            continue;
        }
        const char* ret = _after_kw(text, "return");
        if (ret != NULL) {
            if (strcmp(ret, deny)) {
                continue; /* return True / None: the edit is allowed */
            }
            if (indent == base) {
                mm_dnf_t all = {0};
                all.n        = 1; /* one group with no atom: every cell */
                mm_rules_add(&all);
                rules++;
            } else if (cond_ok && indent > cond_indent) {
                mm_rules_add(&cond);
                rules++;
            } else {
                failed = 1;
            }
            continue;
        }
        if (mm_starts_kw(text, "global") || mm_starts_kw(text, "pass")) {
            continue;
        }
        failed = 1;
    }
    return rules > 0 && !failed;
}
