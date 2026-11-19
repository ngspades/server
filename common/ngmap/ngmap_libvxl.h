/*
 * An ngmap stream decoded straight into a libvxl map, header only:
 * both programs keep their map in libvxl (common/vxl). ngmap_decode wrote the
 * whole map as VXL bytes for libvxl_create to parse again: at 1024 x 1024 the
 * bytes, their parse and the allocator's leftovers were 70 MiB more at the
 * peak. The map built here is the one libvxl_create made from those bytes,
 * block for block (common/ngmap/tests/libvxl_check.c compares them).
 */
#ifndef NGMAP_LIBVXL_H
#define NGMAP_LIBVXL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ngmap.h"
#include <vxl/libvxl.h>

struct ngmap_libvxl_build {
    struct libvxl_map* map;
    int                want_w, want_h, want_d; /* 0: any */
    int                failed;
};

/* libvxl's block key, 0xYYYXXXZZ (its pos_key: the client's minheap.h
   takes that name for another layout) */
static inline uint32_t ngmap_libvxl_key(uint32_t x, uint32_t y, uint32_t z)
{
    return y << 20 | x << 8 | z;
}

static inline struct libvxl_chunk* ngmap_libvxl_chunk(struct libvxl_map* map, size_t x, size_t y)
{
    size_t across = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
    return map->chunks + x / LIBVXL_CHUNK_SIZE + y / LIBVXL_CHUNK_SIZE * across;
}

static inline int ngmap_libvxl_put(struct libvxl_chunk* chunk, uint32_t position, uint32_t color)
{
    if (chunk->index == chunk->length) {
        size_t               length = chunk->length ? chunk->length * LIBVXL_CHUNK_GROWTH : 256;
        struct libvxl_block* blocks = (struct libvxl_block*) realloc(chunk->blocks, length * sizeof *blocks);
        if (blocks == NULL) {
            return 0;
        }
        chunk->blocks = blocks;
        chunk->length = length;
    }
    chunk->blocks[chunk->index].position = position;
    chunk->blocks[chunk->index].color    = color;
    chunk->index++;
    return 1;
}

static inline int ngmap_libvxl_size(void* user, int w, int h, int d)
{
    struct ngmap_libvxl_build* b = (struct ngmap_libvxl_build*) user;
    if ((b->want_w && w != b->want_w) || (b->want_h && h != b->want_h) || (b->want_d && d != b->want_d)) {
        return 1;
    }
    struct libvxl_map* map = b->map;
    memset(map, 0, sizeof *map);
    map->width     = (size_t) w;
    map->height    = (size_t) h;
    map->depth     = (size_t) d;
    size_t chunks  = ((map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE) *
                    ((map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE);
    size_t words   = (map->width * map->height * map->depth + (sizeof(size_t) * 8 - 1)) / (sizeof(size_t) * 8);
    map->chunks    = (struct libvxl_chunk*) calloc(chunks, sizeof *map->chunks);
    map->geometry  = (size_t*) calloc(words, sizeof(size_t));
    if (map->chunks == NULL || map->geometry == NULL) {
        b->failed = 1;
        return 1;
    }
    return 0;
}

static inline void ngmap_libvxl_column(void* user, int x, int y, uint64_t solid, uint64_t colored,
                                       const uint32_t* colors)
{
    struct ngmap_libvxl_build* b   = (struct ngmap_libvxl_build*) user;
    struct libvxl_map*         map = b->map;
    if (b->failed) {
        return;
    }
    size_t column = ((size_t) x + (size_t) y * map->width) * map->depth;
    for (uint64_t left = solid; left; left &= left - 1) {
        size_t bit = column + (size_t) __builtin_ctzll(left);
        map->geometry[bit / (sizeof(size_t) * 8)] |= (size_t) 1 << (bit % (sizeof(size_t) * 8));
    }
    /* column after column, z upwards: the chunk's list stays sorted by its
       key (y, x, z), as libvxl_create's */
    struct libvxl_chunk* chunk = ngmap_libvxl_chunk(map, (size_t) x, (size_t) y);
    int                  n     = 0;
    for (uint64_t left = colored; left; left &= left - 1) {
        if (!ngmap_libvxl_put(chunk, ngmap_libvxl_key((uint32_t) x, (uint32_t) y, (uint32_t) __builtin_ctzll(left)), colors[n++])) {
            b->failed = 1;
            return;
        }
    }
}

static inline int ngmap_libvxl_solid(const struct libvxl_map* map, size_t x, size_t y, size_t z)
{
    size_t bit = z + (x + y * map->width) * map->depth;
    return (map->geometry[bit / (sizeof(size_t) * 8)] >> (bit % (sizeof(size_t) * 8))) & 1;
}

/* a colour where none is, kept sorted: libvxl_create's map edge rule */
static inline int ngmap_libvxl_insert(struct libvxl_map* map, size_t x, size_t y, size_t z)
{
    struct libvxl_chunk* chunk = ngmap_libvxl_chunk(map, x, y);
    uint32_t             key   = ngmap_libvxl_key((uint32_t) x, (uint32_t) y, (uint32_t) z);
    size_t               lo = 0, hi = chunk->index;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (chunk->blocks[mid].position < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < chunk->index && chunk->blocks[lo].position == key) {
        return 1;
    }
    if (!ngmap_libvxl_put(chunk, 0, 0)) {
        return 0;
    }
    memmove(chunk->blocks + lo + 1, chunk->blocks + lo, (chunk->index - 1 - lo) * sizeof *chunk->blocks);
    chunk->blocks[lo].position = key;
    chunk->blocks[lo].color    = DEFAULT_COLOR(x, y, z);
    return 1;
}

/* a chunk with no colour gets libvxl_create's 512: libvxl grows a chunk by
   doubling its length, and a length of 0 stays 0 */
static inline int ngmap_libvxl_empty_chunks(struct libvxl_map* map)
{
    size_t chunks = ((map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE) *
                    ((map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE);
    for (size_t k = 0; k < chunks; k++) {
        if (map->chunks[k].length == 0) {
            map->chunks[k].blocks = (struct libvxl_block*) malloc(512 * sizeof(struct libvxl_block));
            if (map->chunks[k].blocks == NULL) {
                return 0;
            }
            map->chunks[k].length = 512;
        }
    }
    return 1;
}

/* libvxl_create's last pass: a solid cell on the map's edge whose neighbour
   across the wrap is air is a surface, and gets the default colour */
static inline int ngmap_libvxl_edges(struct libvxl_map* map)
{
    size_t w = map->width, h = map->height;
    for (size_t z = 0; z < map->depth; z++) {
        for (size_t x = 0; x < w; x++) {
            int a = ngmap_libvxl_solid(map, x, 0, z), b = ngmap_libvxl_solid(map, x, h - 1, z);
            if ((a && !b && !ngmap_libvxl_insert(map, x, 0, z)) || (!a && b && !ngmap_libvxl_insert(map, x, h - 1, z))) {
                return 0;
            }
        }
        for (size_t y = 0; y < h; y++) {
            int a = ngmap_libvxl_solid(map, 0, y, z), b = ngmap_libvxl_solid(map, w - 1, y, z);
            if ((a && !b && !ngmap_libvxl_insert(map, 0, y, z)) || (!a && b && !ngmap_libvxl_insert(map, w - 1, y, z))) {
                return 0;
            }
        }
    }
    return 1;
}

/* the stream into *map (left untouched on failure, else freed with
   libvxl_free); want_* the size it must be, 0 any. NGMAP_ERR_ARGS for a map
   of another size */
static inline int ngmap_decode_libvxl(const uint8_t* data, size_t size, struct libvxl_map* map, int want_w,
                                      int want_h, int want_d)
{
    struct libvxl_map          built;
    struct ngmap_libvxl_build b = {&built, want_w, want_h, want_d, 0};
    memset(&built, 0, sizeof built);
    int err = ngmap_decode_columns(data, size, ngmap_libvxl_size, ngmap_libvxl_column, &b);
    if (err == NGMAP_OK && (b.failed || !ngmap_libvxl_edges(&built) || !ngmap_libvxl_empty_chunks(&built))) {
        err = NGMAP_ERR_NOMEM;
    }
    if (err != NGMAP_OK) {
        if (built.chunks != NULL) {
            size_t chunks = ((built.width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE) *
                            ((built.height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE);
            for (size_t k = 0; k < chunks; k++) {
                free(built.chunks[k].blocks);
            }
        }
        free(built.chunks);
        free(built.geometry);
        return err;
    }
    *map = built;
    return NGMAP_OK;
}

#endif
