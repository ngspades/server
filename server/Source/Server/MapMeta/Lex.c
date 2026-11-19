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

static pool_block_t* pool;
mm_line_t* mm_lines;
int        mm_line_count;
int        mm_line_cap;

/* ------------------------------------------------------------------ */
/* memory: everything the parser makes lives until the end of the load  */
/* ------------------------------------------------------------------ */


void* mm_alloc(size_t n)
{
    n = (n + 15) & ~(size_t) 15;
    if (pool == NULL || pool->used + n > pool->size) {
        size_t        size  = n > 65536 ? n : 65536;
        pool_block_t* block = spadesx_calloc(1, sizeof(pool_block_t) + size);
        block->next         = pool;
        block->size         = size;
        pool                = block;
    }
    void* p = pool->data + pool->used;
    pool->used += n;
    memset(p, 0, n);
    return p;
}

void mm_pool_free(void)
{
    while (pool != NULL) {
        pool_block_t* next = pool->next;
        free(pool);
        pool = next;
    }
}

char* mm_strndup(const char* s, size_t n)
{
    char* out = mm_alloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* logical mm_lines                                                        */
/* ------------------------------------------------------------------ */



static void _push_line(int indent, const char* text, size_t len)
{
    while (len > 0 && isspace((unsigned char) text[len - 1])) {
        len--;
    }
    while (len > 0 && isspace((unsigned char) *text)) {
        text++;
        len--;
    }
    if (len == 0) {
        return;
    }
    if (mm_line_count == mm_line_cap) {
        int        cap  = mm_line_cap ? mm_line_cap * 2 : 256;
        mm_line_t* grow = mm_alloc(sizeof(mm_line_t) * cap);
        if (mm_line_count > 0) {
            memcpy(grow, mm_lines, sizeof(mm_line_t) * mm_line_count);
        }
        mm_lines    = grow;
        mm_line_cap = cap;
    }
    mm_lines[mm_line_count].indent = indent;
    mm_lines[mm_line_count].text   = mm_strndup(text, len);
    mm_line_count++;
}

/* index just past the string starting at s[i] (a quote), 0 if unclosed */
size_t mm_skip_string(const char* s, size_t i, size_t n)
{
    char q      = s[i];
    int  triple = i + 2 < n && s[i + 1] == q && s[i + 2] == q;
    i += triple ? 3 : 1;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (s[i] == q && (!triple || (i + 2 < n && s[i + 1] == q && s[i + 2] == q))) {
            return i + (triple ? 3 : 1);
        }
        if (s[i] == '\n' && !triple) {
            return i;
        }
        i++;
    }
    return n;
}

/* the colon ending the header of a compound statement, -1 if none */
static int _top_colon(const char* s)
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
        } else if (c == ':' && depth == 0) {
            return (int) i;
        }
    }
    return -1;
}

int mm_starts_kw(const char* s, const char* kw)
{
    size_t n = strlen(kw);
    return strncmp(s, kw, n) == 0 && !(isalnum((unsigned char) s[n]) || s[n] == '_');
}

static const char* const compound_kw[] = {"if", "elif", "else", "for", "while", "def", "class", "with", "try", "except",
                                          "finally"};

int mm_is_compound(const char* s)
{
    for (size_t k = 0; k < sizeof(compound_kw) / sizeof(compound_kw[0]); k++) {
        if (mm_starts_kw(s, compound_kw[k])) {
            return 1;
        }
    }
    return 0;
}

/* "if c: return False" -> "if c:" and "return False" one level deeper */
static void _emit_statement(int indent, const char* text, size_t len)
{
    char* line = mm_strndup(text, len);
    while (*line && isspace((unsigned char) *line)) {
        line++;
    }
    int colon = mm_is_compound(line) ? _top_colon(line) : -1;
    if (colon >= 0 && line[colon + 1] != '\0') {
        const char* rest = line + colon + 1;
        while (*rest && isspace((unsigned char) *rest)) {
            rest++;
        }
        if (*rest) {
            _push_line(indent, line, (size_t) colon + 1);
            _emit_statement(indent + 1, rest, strlen(rest));
            return;
        }
    }
    _push_line(indent, line, strlen(line));
}

void mm_split_lines(const char* src, size_t n)
{
    char*  buf     = mm_alloc(n + 1);
    size_t len     = 0;
    int    depth   = 0;
    int    indent  = 0;
    int    measure = 1;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (measure) {
            if (c == ' ') {
                indent++;
                continue;
            }
            if (c == '\t') {
                indent = (indent / 8 + 1) * 8; /* Python 2 tab stops: Bikini_Bottom mixes them */
                continue;
            }
            if (c == '\r' || c == '\f') {
                continue;
            }
            if (c == '\n') {
                indent = 0;
                continue;
            }
            measure = 0;
        }
        if (c == '\'' || c == '"') {
            size_t end = mm_skip_string(src, i, n);
            for (size_t k = i; k < end; k++) {
                buf[len++] = src[k] == '\n' ? ' ' : src[k];
            }
            i = end - 1;
        } else if (c == '#') {
            while (i + 1 < n && src[i + 1] != '\n') {
                i++;
            }
        } else if (c == '\\' && i + 1 < n && (src[i + 1] == '\n' || src[i + 1] == '\r')) {
            while (i + 1 < n && src[i + 1] != '\n') {
                i++;
            }
            i++;
            buf[len++] = ' ';
        } else if (c == '\n') {
            if (depth > 0) {
                buf[len++] = ' ';
                continue;
            }
            _emit_statement(indent, buf, len);
            len     = 0;
            indent  = 0;
            measure = 1;
        } else if (c == '\r') {
            continue;
        } else {
            if (c == '(' || c == '[' || c == '{') {
                depth++;
            } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
                depth--;
            }
            buf[len++] = c == '\t' ? ' ' : c;
        }
    }
    _emit_statement(indent, buf, len);
}

/* ------------------------------------------------------------------ */
/* tokens                                                               */
/* ------------------------------------------------------------------ */


static const char* const ops2[] = {"==", "!=", "<=", ">=", "//", "**", "+=", "-=", "*=", "/=", "<>"};

tok_t* mm_tokenize(const char* s)
{
    size_t n     = strlen(s);
    tok_t* toks  = mm_alloc(sizeof(tok_t) * (n + 2));
    int    count = 0;
    size_t i     = 0;
    while (i < n) {
        char c = s[i];
        if (isspace((unsigned char) c)) {
            i++;
            continue;
        }
        tok_t* t = &toks[count++];
        if (isalpha((unsigned char) c) || c == '_') {
            size_t start = i;
            while (i < n && (isalnum((unsigned char) s[i]) || s[i] == '_')) {
                i++;
            }
            /* u'..', r'..', b'..': the prefix is part of the string */
            if (i < n && (s[i] == '\'' || s[i] == '"') && i - start <= 2 &&
                strchr("uUrRbB", s[start]) != NULL && (i - start == 1 || strchr("uUrRbB", s[start + 1]) != NULL))
            {
                count--;
                continue;
            }
            t->t = T_NAME;
            t->s = mm_strndup(s + start, i - start);
        } else if (isdigit((unsigned char) c) || (c == '.' && i + 1 < n && isdigit((unsigned char) s[i + 1]))) {
            char* end;
            t->t = T_NUM;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                t->num    = (double) strtol(s + i, &end, 16);
                t->is_int = 1;
            } else {
                t->num    = strtod(s + i, &end);
                t->is_int = 1;
                for (const char* k = s + i; k < end; k++) {
                    if (*k == '.' || *k == 'e' || *k == 'E') {
                        t->is_int = 0;
                    }
                }
            }
            i = (size_t) (end - s);
            if (i < n && (s[i] == 'L' || s[i] == 'l')) {
                i++; /* Python 2 long */
            }
        } else if (c == '\'' || c == '"') {
            size_t end    = mm_skip_string(s, i, n);
            int    triple = i + 2 < n && s[i + 1] == c && s[i + 2] == c;
            size_t from   = i + (triple ? 3 : 1);
            size_t to     = end - (triple ? 3 : 1);
            char*  out    = mm_alloc(to > from ? to - from + 1 : 1);
            size_t len    = 0;
            for (size_t k = from; k < to && k < n; k++) {
                if (s[k] == '\\' && k + 1 < to) {
                    k++;
                    out[len++] = s[k] == 'n' ? '\n' : s[k] == 't' ? '\t' : s[k];
                } else {
                    out[len++] = s[k];
                }
            }
            t->t = T_STR;
            t->s = out;
            i    = end;
        } else {
            t->t = T_OP;
            for (size_t k = 0; k < sizeof(ops2) / sizeof(ops2[0]); k++) {
                if (i + 1 < n && s[i] == ops2[k][0] && s[i + 1] == ops2[k][1]) {
                    t->s = ops2[k];
                    i += 2;
                    break;
                }
            }
            if (t->s == NULL) {
                t->s = mm_strndup(s + i, 1);
                i++;
            }
        }
    }
    toks[count].t = T_END;
    toks[count].s = "";
    return toks;
}
