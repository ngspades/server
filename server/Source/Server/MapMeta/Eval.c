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

env_t* mm_globals;
val_t mm_err_value = {.t = V_ERR};

/* ------------------------------------------------------------------ */
/* values and the expression evaluator                                  */
/* ------------------------------------------------------------------ */




static val_t* _val(val_type_t t)
{
    val_t* v = mm_alloc(sizeof(val_t));
    v->t     = t;
    return v;
}

static val_t* _num(double d, uint8_t is_int)
{
    val_t* v  = _val(V_NUM);
    v->num    = d;
    v->is_int = is_int;
    return v;
}

val_t* mm_env_get(env_t* env, const char* name)
{
    for (; env != NULL; env = env->next) {
        if (!strcmp(env->name, name)) {
            return env->v;
        }
    }
    return NULL;
}

void mm_env_set(env_t** env, const char* name, val_t* v)
{
    env_t* e = mm_alloc(sizeof(env_t));
    e->name  = name;
    e->v     = v;
    e->next  = *env;
    *env     = e;
}

tok_t* mm_peek(ps_t* p)
{
    return &p->tok[p->pos];
}

int mm_is_op(ps_t* p, const char* op)
{
    return mm_peek(p)->t == T_OP && !strcmp(mm_peek(p)->s, op);
}

int mm_is_name(ps_t* p, const char* name)
{
    return mm_peek(p)->t == T_NAME && !strcmp(mm_peek(p)->s, name);
}

int mm_accept(ps_t* p, const char* op)
{
    if (mm_is_op(p, op)) {
        p->pos++;
        return 1;
    }
    return 0;
}

val_t* mm_parse_arith(ps_t* p);

/* items up to the closing bracket, trailing comma allowed */
static val_t* _parse_items(ps_t* p, const char* close)
{
    val_t*  seq   = _val(V_SEQ);
    int     cap   = 8;
    val_t** items = mm_alloc(sizeof(val_t*) * cap);
    while (!mm_accept(p, close)) {
        if (mm_peek(p)->t == T_END) {
            return &mm_err_value;
        }
        val_t* item = mm_parse_arith(p);
        if (item->t == V_ERR) {
            return item;
        }
        if (seq->n == cap) {
            val_t** grow = mm_alloc(sizeof(val_t*) * cap * 2);
            memcpy(grow, items, sizeof(val_t*) * cap);
            items = grow;
            cap *= 2;
        }
        items[seq->n++] = item;
        if (!mm_accept(p, ",") && !mm_is_op(p, close)) {
            return &mm_err_value;
        }
    }
    seq->items = items;
    return seq;
}

static val_t* _parse_atom(ps_t* p)
{
    tok_t* t = mm_peek(p);
    if (t->t == T_NUM) {
        p->pos++;
        return _num(t->num, t->is_int);
    }
    if (t->t == T_STR) {
        /* 'a' 'b' is one string */
        size_t len = 0;
        for (int k = p->pos; p->tok[k].t == T_STR; k++) {
            len += strlen(p->tok[k].s);
        }
        char*  s   = mm_alloc(len + 1);
        size_t end = 0;
        while (mm_peek(p)->t == T_STR) {
            size_t n = strlen(mm_peek(p)->s);
            memcpy(s + end, mm_peek(p)->s, n);
            end += n;
            p->pos++;
        }
        s[end] = '\0';
        val_t* v = _val(V_STR);
        v->s     = s;
        return v;
    }
    if (t->t == T_NAME) {
        p->pos++;
        if (!strcmp(t->s, "True") || !strcmp(t->s, "False")) {
            val_t* v  = _val(V_BOOL);
            v->num    = t->s[0] == 'T';
            v->is_int = 1;
            return v;
        }
        if (!strcmp(t->s, "None")) {
            return _val(V_NONE);
        }
        val_t* v = mm_env_get(p->locals, t->s);
        if (v == NULL) {
            v = mm_env_get(mm_globals, t->s);
        }
        if (v != NULL) {
            return v;
        }
        if (!strcmp(t->s, "choice")) { /* from random import choice */
            v     = _val(V_FN);
            v->fn = FN_CHOICE;
            return v;
        }
        v    = _val(V_OBJ);
        v->s = t->s;
        return v;
    }
    if (mm_accept(p, "(")) {
        if (mm_accept(p, ")")) {
            return _val(V_SEQ);
        }
        val_t* first = mm_parse_arith(p);
        if (first->t == V_ERR) {
            return first;
        }
        if (mm_accept(p, ")")) {
            return first;
        }
        if (!mm_accept(p, ",")) {
            return &mm_err_value;
        }
        val_t* rest = _parse_items(p, ")");
        if (rest->t == V_ERR) {
            return rest;
        }
        val_t* seq = _val(V_SEQ);
        seq->n     = rest->n + 1;
        seq->items = mm_alloc(sizeof(val_t*) * seq->n);
        seq->items[0] = first;
        for (int k = 0; k < rest->n; k++) {
            seq->items[k + 1] = rest->items[k];
        }
        return seq;
    }
    if (mm_accept(p, "[")) {
        return _parse_items(p, "]");
    }
    if (mm_accept(p, "{")) {
        val_t* dict = _val(V_DICT);
        int    cap  = 16;
        dict->items = mm_alloc(sizeof(val_t*) * cap);
        dict->keys  = mm_alloc(sizeof(val_t*) * cap);
        while (!mm_accept(p, "}")) {
            if (mm_peek(p)->t == T_END || dict->n == cap) {
                return &mm_err_value;
            }
            val_t* key = mm_parse_arith(p);
            if (key->t == V_ERR || !mm_accept(p, ":")) {
                return &mm_err_value;
            }
            val_t* value = mm_parse_arith(p);
            if (value->t == V_ERR) {
                return value;
            }
            dict->keys[dict->n]  = key;
            dict->items[dict->n] = value;
            dict->n++;
            if (!mm_accept(p, ",") && !mm_is_op(p, "}")) {
                return &mm_err_value;
            }
        }
        return dict;
    }
    return &mm_err_value;
}

static int _val_equal(const val_t* a, const val_t* b)
{
    if (a->t == V_STR && b->t == V_STR) {
        return !strcmp(a->s, b->s);
    }
    if ((a->t == V_NUM || a->t == V_BOOL) && (b->t == V_NUM || b->t == V_BOOL)) {
        return a->num == b->num;
    }
    return 0;
}

static val_t* _call(ps_t* p, val_t* fn, val_t* args)
{
    if (fn->t != V_FN) {
        return &mm_err_value;
    }
    switch (fn->fn) {
        case FN_GET_Z:
        {
            if (args->n != 2 || args->items[0]->t != V_NUM || args->items[1]->t != V_NUM) {
                return &mm_err_value;
            }
            map_store_t* map = &p->server->s_map.map;
            int       x   = (int) args->items[0]->num;
            int       y   = (int) args->items[1]->num;
            if (!map_store_live(map) || x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
                return &mm_err_value;
            }
            return _num(map_top(map, (uint16_t) x, (uint16_t) y), 1);
        }
        case FN_CHOICE:
        {
            if (args->n != 1 || args->items[0]->t != V_SEQ) {
                return &mm_err_value;
            }
            val_t* v = _val(V_CHOICE);
            v->items = args->items;
            v->n     = 1;
            return v;
        }
        case FN_RANDLOC:
        {
            if (args->n < 1) {
                return &mm_err_value;
            }
            val_t* v = _val(V_RANDLOC);
            v->items = args->items + args->n - 1; /* the zone is the last argument */
            v->n     = 1;
            return v;
        }
    }
    return &mm_err_value;
}

static val_t* _parse_postfix(ps_t* p)
{
    val_t* v = _parse_atom(p);
    while (v->t != V_ERR) {
        if (mm_accept(p, ".")) {
            if (mm_peek(p)->t != T_NAME || (v->t != V_OBJ && v->t != V_FN)) {
                return &mm_err_value;
            }
            const char* attr = mm_peek(p)->s;
            p->pos++;
            if (!strcmp(attr, "get_z")) {
                v     = _val(V_FN);
                v->fn = FN_GET_Z;
            } else if (!strcmp(attr, "choice")) {
                v     = _val(V_FN);
                v->fn = FN_CHOICE;
            } else if (!strcmp(attr, "get_random_location")) {
                v     = _val(V_FN);
                v->fn = FN_RANDLOC;
            } else {
                v    = _val(V_OBJ);
                v->s = attr;
            }
        } else if (mm_accept(p, "[")) {
            val_t* index = mm_parse_arith(p);
            if (index->t == V_ERR || !mm_accept(p, "]")) {
                return &mm_err_value;
            }
            if (v->t == V_SEQ && index->t == V_NUM) {
                int k = (int) index->num;
                if (k < 0) {
                    k += v->n;
                }
                if (k < 0 || k >= v->n) {
                    return &mm_err_value;
                }
                v = v->items[k];
            } else if (v->t == V_DICT) {
                val_t* found = NULL;
                for (int k = 0; k < v->n && found == NULL; k++) {
                    if (_val_equal(v->keys[k], index)) {
                        found = v->items[k];
                    }
                }
                if (found == NULL) {
                    return &mm_err_value;
                }
                v = found;
            } else {
                return &mm_err_value;
            }
        } else if (mm_accept(p, "(")) {
            val_t* args = _parse_items(p, ")");
            if (args->t == V_ERR) {
                return args;
            }
            v = _call(p, v, args);
        } else {
            break;
        }
    }
    return v;
}

static val_t* _parse_unary(ps_t* p)
{
    if (mm_accept(p, "-")) {
        val_t* v = _parse_unary(p);
        return v->t == V_NUM ? _num(-v->num, v->is_int) : &mm_err_value;
    }
    if (mm_accept(p, "+")) {
        val_t* v = _parse_unary(p);
        return v->t == V_NUM ? v : &mm_err_value;
    }
    return _parse_postfix(p);
}

static val_t* _binary(const char* op, val_t* a, val_t* b)
{
    if (a->t != V_NUM || b->t != V_NUM) {
        return &mm_err_value;
    }
    uint8_t is_int = a->is_int && b->is_int;
    double  x = a->num, y = b->num;
    switch (op[0]) {
        case '+':
            return _num(x + y, is_int);
        case '-':
            return _num(x - y, is_int);
        case '*':
            return _num(x * y, is_int);
        case '%':
            return y == 0 ? &mm_err_value : _num(x - y * floor(x / y), is_int);
        case '/':
            if (y == 0) {
                return &mm_err_value;
            }
            /* the scripts are Python 2: int / int is a floor division */
            return (is_int || op[1] == '/') ? _num(floor(x / y), is_int) : _num(x / y, 0);
    }
    return &mm_err_value;
}

static val_t* _parse_term(ps_t* p)
{
    val_t* v = _parse_unary(p);
    while (v->t != V_ERR && mm_peek(p)->t == T_OP &&
           (!strcmp(mm_peek(p)->s, "*") || !strcmp(mm_peek(p)->s, "/") || !strcmp(mm_peek(p)->s, "//") ||
            !strcmp(mm_peek(p)->s, "%")))
    {
        const char* op = mm_peek(p)->s;
        p->pos++;
        v = _binary(op, v, _parse_unary(p));
    }
    return v;
}

val_t* mm_parse_arith(ps_t* p)
{
    val_t* v = _parse_term(p);
    while (v->t != V_ERR && (mm_is_op(p, "+") || mm_is_op(p, "-"))) {
        const char* op = mm_peek(p)->s;
        p->pos++;
        v = _binary(op, v, _parse_term(p));
    }
    return v;
}

/* a whole expression, "a, b, c" being a tuple as in a return */
val_t* mm_eval(server_t* server, env_t* locals, const char* text)
{
    ps_t   p     = {mm_tokenize(text), 0, locals, server};
    val_t* first = mm_parse_arith(&p);
    if (first->t == V_ERR) {
        return first;
    }
    if (mm_peek(&p)->t == T_END) {
        return first;
    }
    val_t* seq = _val(V_SEQ);
    seq->items = mm_alloc(sizeof(val_t*) * 64);
    seq->items[seq->n++] = first;
    while (mm_accept(&p, ",") && mm_peek(&p)->t != T_END && seq->n < 64) {
        val_t* item = mm_parse_arith(&p);
        if (item->t == V_ERR) {
            return item;
        }
        seq->items[seq->n++] = item;
    }
    return mm_peek(&p)->t == T_END ? seq : &mm_err_value;
}

/* the numbers of a tuple/list of at least `min` numbers */
int mm_numbers(const val_t* v, int min, double* out, int max)
{
    if (v->t != V_SEQ || v->n < min) {
        return 0;
    }
    int n = v->n < max ? v->n : max;
    for (int k = 0; k < n; k++) {
        if (v->items[k]->t != V_NUM) {
            return 0;
        }
        out[k] = v->items[k]->num;
    }
    return n;
}
