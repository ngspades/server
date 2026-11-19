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
/* conditions over the block coordinates: protected cells               */
/* ------------------------------------------------------------------ */


static int _coord_axis(cond_t* c, const tok_t* t)
{
    for (int k = 0; t->t == T_NAME && k < 3; k++) {
        if (c->coord[k] != NULL && !strcmp(c->coord[k], t->s)) {
            return k;
        }
    }
    return -1;
}

static int _cmp_op(ps_t* p)
{
    static const char* const names[] = {"==", "!=", "<", "<=", ">", ">="};
    for (int k = 0; k < 6; k++) {
        if (mm_is_op(p, names[k])) {
            p->pos++;
            return k;
        }
    }
    return -1;
}

static int _dnf_add(mm_dnf_t* out, const mm_group_t* g)
{
    if (out->n >= MM_MAX_GROUPS) {
        return 0;
    }
    out->g[out->n++] = *g;
    return 1;
}

static int _cond_or(cond_t* c, mm_dnf_t* out);

static int _cond_atom(cond_t* c, mm_dnf_t* out)
{
    ps_t* p = &c->p;
    out->n  = 0;
    if (mm_is_op(p, "(")) {
        int save = p->pos;
        p->pos++;
        if (_cond_or(c, out) && mm_accept(p, ")")) {
            return 1;
        }
        p->pos = save;
        out->n = 0;
    }
    int axis = _coord_axis(c, mm_peek(p));
    if (axis >= 0) {
        p->pos++;
        if (mm_is_name(p, "in")) {
            p->pos++;
            val_t* set = mm_parse_arith(p);
            if (set->t != V_SEQ) {
                return 0;
            }
            for (int k = 0; k < set->n; k++) {
                if (set->items[k]->t != V_NUM) {
                    return 0;
                }
                mm_group_t g = {1, {{(uint8_t) axis, OP_EQ, (float) set->items[k]->num}}};
                if (!_dnf_add(out, &g)) {
                    return 0;
                }
            }
            return 1;
        }
        int op = _cmp_op(p);
        if (op < 0) {
            return 0;
        }
        val_t* v = mm_parse_arith(p);
        if (v->t != V_NUM) {
            return 0;
        }
        mm_group_t g = {1, {{(uint8_t) axis, (uint8_t) op, (float) v->num}}};
        return _dnf_add(out, &g);
    }
    /* constant on the left: 95 == x */
    val_t* v  = mm_parse_arith(p);
    int    op = _cmp_op(p);
    axis      = _coord_axis(c, mm_peek(p));
    if (v->t != V_NUM || op < 0 || axis < 0) {
        return 0;
    }
    p->pos++;
    static const uint8_t flip[] = {OP_EQ, OP_NE, OP_GT, OP_GE, OP_LT, OP_LE};
    mm_group_t           g      = {1, {{(uint8_t) axis, flip[op], (float) v->num}}};
    return _dnf_add(out, &g);
}

static int _cond_and(cond_t* c, mm_dnf_t* out)
{
    if (!_cond_atom(c, out)) {
        return 0;
    }
    while (mm_is_name(&c->p, "and")) {
        c->p.pos++;
        mm_dnf_t right, product = {0};
        if (!_cond_atom(c, &right)) {
            return 0;
        }
        /* (a or b) and (c or d) = ac or ad or bc or bd */
        for (int i = 0; i < out->n; i++) {
            for (int j = 0; j < right.n; j++) {
                mm_group_t g = out->g[i];
                if (g.n + right.g[j].n > MM_MAX_ATOMS) {
                    return 0;
                }
                for (int k = 0; k < right.g[j].n; k++) {
                    g.a[g.n++] = right.g[j].a[k];
                }
                if (!_dnf_add(&product, &g)) {
                    return 0;
                }
            }
        }
        *out = product;
    }
    return 1;
}

static int _cond_or(cond_t* c, mm_dnf_t* out)
{
    if (!_cond_and(c, out)) {
        return 0;
    }
    while (mm_is_name(&c->p, "or")) {
        c->p.pos++;
        mm_dnf_t right;
        if (!_cond_and(c, &right)) {
            return 0;
        }
        for (int k = 0; k < right.n; k++) {
            if (!_dnf_add(out, &right.g[k])) {
                return 0;
            }
        }
    }
    return 1;
}

int mm_parse_cond(server_t* server, const char* text, const char* coord[3], mm_dnf_t* out)
{
    cond_t c = {{mm_tokenize(text), 0, NULL, server}, {coord[0], coord[1], coord[2]}};
    return _cond_or(&c, out) && mm_peek(&c.p)->t == T_END;
}

static int _group_equal(const mm_group_t* a, const mm_group_t* b)
{
    if (a->n != b->n) {
        return 0;
    }
    for (int k = 0; k < a->n; k++) {
        if (a->a[k].axis != b->a[k].axis || a->a[k].op != b->a[k].op || a->a[k].v != b->a[k].v) {
            return 0;
        }
    }
    return 1;
}

/* Busan_Port protects the same mm_lines in on_block_destroy and in
   on_block_build_attempt: keep each rule once */
void mm_rules_add(const mm_dnf_t* dnf)
{
    for (int i = 0; i < dnf->n; i++) {
        int known = 0;
        for (int j = 0; j < mm_meta.rules.n && !known; j++) {
            known = _group_equal(&mm_meta.rules.g[j], &dnf->g[i]);
        }
        if (!known && mm_meta.rules.n < MM_MAX_GROUPS) {
            mm_meta.rules.g[mm_meta.rules.n++] = dnf->g[i];
        }
    }
}
