// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A libvxl map written as AoS VXL, a column at a time, header only.
 *
 * libvxl's own writer (libvxl_write, libvxl_stream) never ends a column with
 * no solid cell: its span loop runs on and writes past any buffer. 25 maps of
 * the maps repository have such columns (Empty_ocean, which the server ships,
 * 10yearsaloha, CheckerRace...), so a 0.75 join, a 1.0 VXL transfer and the
 * client's F9 save crashed on them. This writer follows ngmap's WriteVxl
 * (common/ngmap/vxl.cpp), the spans pysnip and OpenSpades write: every solid
 * run's top and bottom cell carries a colour (the default brown where libvxl
 * kept none) but the bottom of the run on the map's floor, and a coloured cell inside a run starts a span with no air,
 * which every VXL reader takes. An all-air column is one last span, S = depth
 * and no colour, as the real maps store it.
 *
 * Columns go in the file's order (y, then x), each from libvxl's bitset and
 * its chunk's sorted colour list, read with a cursor a chunk as libvxl's
 * stream does. A column writes at most VXLWRITE_COLUMN_MAX bytes: each span
 * takes at least one cell, so a column has at most depth spans of a 4 byte
 * header, and at most depth colours of 4 bytes.
 */
#ifndef VXLWRITE_H
#define VXLWRITE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vxl/libvxl.h>

#define VXLWRITE_COLUMN_MAX(depth) ((size_t) 4 * (2 * (size_t) (depth) + 1))

/* the most a whole map writes */
static inline size_t vxlwrite_bound(const struct libvxl_map* map)
{
    return map->width * map->height * VXLWRITE_COLUMN_MAX(map->depth);
}

struct vxlwrite {
    struct libvxl_map* map;
    size_t*            cursor; /* a chunk's next colour block */
    size_t             column; /* the next column, y * width + x */
};

/* 1 on success; vxlwrite_end frees it */
static inline int vxlwrite_begin(struct vxlwrite* w, struct libvxl_map* map)
{
    size_t chunks = ((map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE) *
                    ((map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE);
    w->map    = map;
    w->column = 0;
    w->cursor = (size_t*) calloc(chunks ? chunks : 1, sizeof(size_t));
    return w->cursor != NULL;
}

static inline void vxlwrite_end(struct vxlwrite* w)
{
    free(w->cursor);
    w->cursor = NULL;
}

static inline int vxlwrite_done(const struct vxlwrite* w)
{
    return w->column >= w->map->width * w->map->height;
}

static inline void vxlwrite_put_color(uint8_t* out, uint32_t color)
{
    /* the bytes are B, G, R, A: the colour read as a little-endian u32, the
       alpha byte 0x7F as libvxl writes it */
    out[0] = (uint8_t) color;
    out[1] = (uint8_t) (color >> 8);
    out[2] = (uint8_t) (color >> 16);
    out[3] = 0x7F;
}

/* the next column into out (VXLWRITE_COLUMN_MAX(depth) bytes free); the
   bytes written */
static inline size_t vxlwrite_column(struct vxlwrite* w, uint8_t* out)
{
    struct libvxl_map* map   = w->map;
    size_t             x     = w->column % map->width, y = w->column / map->width;
    size_t             d     = map->depth;
    size_t             first = (x + y * map->width) * d;
    w->column++;

    /* the column's solid cells, bit z for cell z (libvxl keeps them in a
       row of the bitset: one word when the map is 64 deep) */
    uint64_t solid = 0;
    if (d == 64 && sizeof(size_t) == 8) {
        solid = (uint64_t) map->geometry[first / 64];
    } else {
        for (size_t z = 0; z < d; z++) {
            size_t bit = first + z;
            solid |= (uint64_t) ((map->geometry[bit / (sizeof(size_t) * 8)] >> (bit % (sizeof(size_t) * 8))) & 1) << z;
        }
    }

    /* its colours: the chunk's blocks of this column, in z order */
    uint32_t             colors[64];
    uint64_t             colored = 0;
    size_t               across  = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
    size_t               k       = x / LIBVXL_CHUNK_SIZE + y / LIBVXL_CHUNK_SIZE * across;
    struct libvxl_chunk* chunk   = map->chunks + k;
    /* libvxl's key is 0xYYYXXXZZ (its pos_key, which the client's
       minheap.h takes for another layout) */
    uint32_t             key     = (uint32_t) y << 20 | (uint32_t) x << 8;
    while (w->cursor[k] < chunk->index && (chunk->blocks[w->cursor[k]].position & ~0xFFu) < key) {
        w->cursor[k]++; /* a block of a column already written: none, sorted */
    }
    while (w->cursor[k] < chunk->index && (chunk->blocks[w->cursor[k]].position & ~0xFFu) == key) {
        uint32_t z = chunk->blocks[w->cursor[k]].position & 0xFFu;
        if (z < d && (solid >> z & 1)) {
            colored |= (uint64_t) 1 << z;
            colors[z] = chunk->blocks[w->cursor[k]].color;
        }
        w->cursor[k]++;
    }
    /* a run's top and bottom need a colour in VXL, but for the bottom of a
       run that reaches the map's bottom: the last span is solid down to it
       with no colour (a colour there, which libvxl keeps none of, was a
       span more a column: Raindance's VXL 2.1 MB to 4.2) */
    uint64_t bottom = (uint64_t) 1 << (d - 1);
    uint64_t need   = (solid & ~(solid << 1)) | (solid & ~(solid >> 1) & ~bottom);
    for (uint64_t left = need & ~colored; left; left &= left - 1) {
        int z     = __builtin_ctzll(left);
        colors[z] = DEFAULT_COLOR(x, y, z);
    }
    uint64_t c = colored | need;

#define VXLWRITE_SOLID(z) ((z) < d && ((solid >> (z)) & 1))
#define VXLWRITE_SURF(z) ((z) < d && ((c >> (z)) & 1))
    size_t n = 0, z = 0;
    while (z < d) {
        size_t air = z;
        while (z < d && !VXLWRITE_SOLID(z)) {
            z++;
        }
        size_t top_start = z;
        while (VXLWRITE_SURF(z)) {
            z++;
        }
        size_t top_end = z;
        while (VXLWRITE_SOLID(z) && !VXLWRITE_SURF(z)) {
            z++;
        }
        size_t bottom_start = z, next = z;
        while (VXLWRITE_SURF(next)) {
            next++;
        }
        if (next != d) {
            while (VXLWRITE_SURF(z)) {
                z++;
            }
        }
        size_t top_len = top_end - top_start, bottom_len = z - bottom_start;
        out[n]     = z == d ? 0 : (uint8_t) (top_len + bottom_len + 1);
        out[n + 1] = (uint8_t) top_start;
        out[n + 2] = (uint8_t) (top_end - 1);
        out[n + 3] = (uint8_t) air;
        n += 4;
        for (size_t i = 0; i < top_len; i++, n += 4) {
            vxlwrite_put_color(out + n, colors[top_start + i]);
        }
        for (size_t i = 0; i < bottom_len; i++, n += 4) {
            vxlwrite_put_color(out + n, colors[bottom_start + i]);
        }
    }
#undef VXLWRITE_SOLID
#undef VXLWRITE_SURF
    return n;
}

/* the whole map into out (vxlwrite_bound bytes); its size, 0 when out of
   memory */
static inline size_t vxlwrite_map(struct libvxl_map* map, uint8_t* out)
{
    struct vxlwrite w;
    if (!vxlwrite_begin(&w, map)) {
        return 0;
    }
    size_t n = 0;
    while (!vxlwrite_done(&w)) {
        n += vxlwrite_column(&w, out + n);
    }
    vxlwrite_end(&w);
    return n;
}

#endif
