// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAPMETA_INTERNAL_H
#define MAPMETA_INTERNAL_H

/*
 * What the pieces of the pyspades map script reader share (Lex.c: lines
 * and tokens, Eval.c: Python values and expressions, Cond.c:
 * protected-cell conditions, Func.c: the script's functions, MapMeta.c:
 * the load and the answers). Nothing here is for any other file.
 */

#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>
#include <stddef.h>
#include <stdint.h>

#define MM_MAX_POINTS  32
#define MM_MAX_SECTORS 64
#define MM_MAX_GROUPS  32
#define MM_MAX_ATOMS   4
#define MM_MAX_GATES   32
#define MM_SECTOR      64 /* pyspades sectors: A..H x 1..8, 64 blocks each */

enum
{
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
};

/* axis (0 x, 1 y, 2 z) op value; a group protects when all its atoms hold */
typedef struct
{
    uint8_t axis;
    uint8_t op;
    float   v;
} mm_atom_t;

typedef struct
{
    int       n;
    mm_atom_t a[MM_MAX_ATOMS];
} mm_group_t;

typedef struct
{
    int        n;
    mm_group_t g[MM_MAX_GROUPS];
} mm_dnf_t;

typedef struct
{
    uint8_t    loaded;
    char       name[64];
    char       version[32];
    char       author[96];
    char       description[192];
    uint8_t    has_entity[2][2];
    vector3f_t entity[2][2];
    int        point_count[2];
    vector3f_t points[2][MM_MAX_POINTS];
    uint8_t    has_rect[2];
    float      rect[2][4];
    uint8_t    has_boundary;
    float      bound[4]; /* left, right, top, bottom */
    int        boundary_damage;
    uint8_t    has_water;
    int        water_damage;
    uint8_t    has_fog;
    uint8_t    fog[3];
    uint8_t    has_heavens;
    uint32_t   heavens;
    int        sector_count;
    int        sectors[MM_MAX_SECTORS][2];
    mm_dnf_t   rules;
    int        gate_count;
    int        gates[MM_MAX_GATES][3];
    /* the arena script's own settings: -1 and 0 when the map says nothing */
    int   arena_spawn_change;
    float arena_max_spawn_distance;
} mm_meta_t;
typedef struct pool_block
{
    struct pool_block* next;
    size_t             used;
    size_t             size;
    char               data[];
} pool_block_t;
typedef struct
{
    int   indent;
    char* text;
} mm_line_t;
typedef enum
{
    T_END,
    T_NAME,
    T_NUM,
    T_STR,
    T_OP,
} tok_type_t;

typedef struct
{
    tok_type_t  t;
    const char* s;
    double      num;
    uint8_t     is_int;
} tok_t;
typedef enum
{
    V_ERR,
    V_NONE,
    V_BOOL,
    V_NUM,
    V_STR,
    V_SEQ,
    V_DICT,
    V_OBJ,     /* a name the evaluator does not know: only its attributes matter */
    V_FN,      /* a recognised function, not called yet */
    V_CHOICE,  /* random.choice(seq) */
    V_RANDLOC, /* protocol.get_random_location(force_land, rect) */
} val_type_t;

typedef enum
{
    FN_GET_Z,
    FN_CHOICE,
    FN_RANDLOC,
} fn_t;

typedef struct val
{
    val_type_t   t;
    double       num;
    uint8_t      is_int;
    const char*  s;
    int          n;
    struct val** items;
    struct val** keys;
    fn_t         fn;
} val_t;

typedef struct env
{
    const char* name;
    val_t*      v;
    struct env* next;
} env_t;

typedef struct
{
    tok_t*    tok;
    int       pos;
    env_t*    locals;
    server_t* server;
} ps_t;
typedef struct
{
    ps_t        p;
    const char* coord[3]; /* the function's names for x, y, z */
} cond_t;
typedef struct
{
    const char* name;
    const char* params[8];
    int         nparams;
    mm_line_t*  body;
    int         count;
} mm_func_t;

/* shared state and functions */
extern mm_line_t* mm_lines;
extern int        mm_line_count;
extern int        mm_line_cap;
extern env_t* mm_globals;
extern val_t mm_err_value;
extern mm_meta_t mm_meta;

void* mm_alloc(size_t n);
void mm_pool_free(void);
char* mm_strndup(const char* s, size_t n);
size_t mm_skip_string(const char* s, size_t i, size_t n);
int mm_starts_kw(const char* s, const char* kw);
int mm_is_compound(const char* s);
void mm_split_lines(const char* src, size_t n);
tok_t* mm_tokenize(const char* s);
val_t* mm_env_get(env_t* env, const char* name);
void mm_env_set(env_t** env, const char* name, val_t* v);
tok_t* mm_peek(ps_t* p);
int mm_is_op(ps_t* p, const char* op);
int mm_is_name(ps_t* p, const char* name);
int mm_accept(ps_t* p, const char* op);
val_t* mm_parse_arith(ps_t* p);
val_t* mm_eval(server_t* server, env_t* locals, const char* text);
int mm_numbers(const val_t* v, int min, double* out, int max);
int mm_parse_cond(server_t* server, const char* text, const char* coord[3], mm_dnf_t* out);
void mm_rules_add(const mm_dnf_t* dnf);
int mm_assign_pos(const char* s);
int mm_is_identifier(const char* s, size_t n);
int mm_do_entity_location(server_t* server, const mm_func_t* f);
void mm_add_point(int team, const double xyz[3]);
int mm_do_spawn_location(server_t* server, const mm_func_t* f);
int mm_do_protection(server_t* server, const mm_func_t* f, const char* deny);

#endif
