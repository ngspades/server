// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAPSTORE_H
#define MAPSTORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <maphash/maphash.h>
#include <ngmap/ngmap_libvxl.h>
#include <vxl/libvxl.h>
#include <vxlwrite/vxlwrite.h>

/* Every access the server makes to the map goes through here, on
   libvxl, the client's map library (common/vxl): the solid cells as a
   bitset, a colour only for the cells that touch the air, the only ones a
   VXL file has one for. 6.5 to 9 MiB at 512 and about 32 MiB at 1024,
   where SpadesX's dense libmapvxl, 4 bytes a cell, held 64 and 256 MiB
   (docs/map-memory.md); is_solid three times faster at 1024.

   The answers are the ones libmapvxl gave, so a match plays event for
   event the same (tools/bench.py check): a colour is 0xFFRRGGBB, an empty
   cell's the default dirt colour, a column with no solid cell has its top
   at 0. One difference is libvxl's own: a cell buried by a block built
   against it loses its colour, and the default dirt shows if it is dug out
   again, as on every client (they run libvxl too and are told only of the
   cell that changed), so a late joiner now sees what the others see.

   Every call takes any coordinates. The bots and the CTF goals look round
   points that may be off a small map; such a read crashed the server on 14
   maps of the maps repository. A coordinate cast to uint16_t from a
   negative number arrives as 65535 and is off the map too. Off the map is
   air (colour 0), a column off it has its top at 0, z = size_z is air, a
   write off it does nothing. The sizes are size_x, size_y and size_z. */
typedef struct map_store {
    uint16_t          size_x;
    uint16_t          size_y;
    uint16_t          size_z;
    uint8_t           live; /* created and not freed */
    /* a map loaded (the high 32 bits) and every change to it (the low ones)
       has a version of its own: what is made from the map (the 0.75
       profile's compressed VXL) is good while the version is the same */
    uint64_t          version;
    struct libvxl_map vxl;
} map_store_t;

static inline void map_store_new_version(map_store_t* map)
{
    map->version = ((map->version >> 32) + 1) << 32;
}

#define MAP_STORE_DEFAULT_COLOR 0xFF674028u

static inline int map_inside(const map_store_t* map, int x, int y, int z)
{
    return x >= 0 && y >= 0 && z >= 0 && x < map->size_x && y < map->size_y && z < map->size_z;
}

static inline int map_store_live(const map_store_t* map)
{
    return map->live;
}

/* an empty map of that size (its bottom layer solid, as libvxl makes one);
   1 on success */
static inline int map_store_create(map_store_t* map, int size_x, int size_y, int size_z)
{
    uint64_t version = map->version;
    memset(map, 0, sizeof *map);
    map->version = version;
    map_store_new_version(map);
    if (!libvxl_create(&map->vxl, (size_t) size_x, (size_t) size_y, (size_t) size_z, NULL, 0)) {
        return 0;
    }
    map->size_x = (uint16_t) size_x;
    map->size_y = (uint16_t) size_y;
    map->size_z = (uint16_t) size_z;
    map->live   = 1;
    return 1;
}

static inline void map_store_free(map_store_t* map)
{
    if (map->live) {
        libvxl_free(&map->vxl);
    }
    map->live = 0;
}

/* the map from VXL bytes, at the size it was created with; 0 when the bytes
   are not a map of that size */
static inline int map_store_read(map_store_t* map, const uint8_t* vxl, size_t len)
{
    int size_x = map->size_x, size_y = map->size_y, size_z = map->size_z;
    map_store_free(map);
    map_store_new_version(map);
    int ok = libvxl_create(&map->vxl, (size_t) size_x, (size_t) size_y, (size_t) size_z, vxl, len);
    if (!ok) {
        /* a half read map is no map: an empty one of that size instead */
        libvxl_free(&map->vxl);
        libvxl_create(&map->vxl, (size_t) size_x, (size_t) size_y, (size_t) size_z, NULL, 0);
    }
    map->size_x = (uint16_t) size_x;
    map->size_y = (uint16_t) size_y;
    map->size_z = (uint16_t) size_z;
    map->live   = 1;
    return ok;
}

/* the map from an ngmap stream, decoded straight into it (no VXL bytes in
   between: common/ngmap/ngmap_libvxl.h), at the size it was created with;
   NGMAP_OK, else the error and the map is left as it was */
static inline int map_store_read_ngmap(map_store_t* map, const uint8_t* data, size_t len)
{
    struct libvxl_map built;
    int err = ngmap_decode_libvxl(data, len, &built, map->size_x, map->size_y, map->size_z);
    if (err == NGMAP_OK) {
        libvxl_free(&map->vxl);
        map->vxl = built;
        map_store_new_version(map);
    }
    return err;
}

/* the most map_store_write may write (common/vxlwrite: a column holds up to
   size_z spans) */
static inline size_t map_store_write_bound(const map_store_t* map)
{
    return vxlwrite_bound(&map->vxl);
}

/* the map as VXL bytes into out (map_store_write_bound bytes); the size.
   Not libvxl_write: it never ends an all-air column (Empty_ocean's) and
   writes past any buffer */
static inline size_t map_store_write(map_store_t* map, uint8_t* out)
{
    return vxlwrite_map(&map->vxl, out);
}

/* dst becomes a copy of src (dst created at the same size) */
static inline void map_store_copy(map_store_t* dst, const map_store_t* src)
{
    const struct libvxl_map* s = &src->vxl;
    struct libvxl_map*       d = &dst->vxl;
    size_t bits = (s->width * s->height * s->depth + (sizeof(size_t) * 8 - 1)) / (sizeof(size_t) * 8);
    memcpy(d->geometry, s->geometry, bits * sizeof(size_t));
    size_t chunks = ((s->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE) *
                    ((s->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE);
    for (size_t i = 0; i < chunks; i++) {
        if (d->chunks[i].length < s->chunks[i].index) {
            d->chunks[i].blocks = (struct libvxl_block*) realloc(d->chunks[i].blocks,
                                                                 s->chunks[i].length * sizeof(struct libvxl_block));
            d->chunks[i].length = s->chunks[i].length;
        }
        memcpy(d->chunks[i].blocks, s->chunks[i].blocks, s->chunks[i].index * sizeof(struct libvxl_block));
        d->chunks[i].index = s->chunks[i].index;
    }
}

/* the column's solid cells, bit z for cell z (a map at most 64 deep); 0 off
   the map. libvxl keeps a column's 64 cells in one word of its bitset */
static inline uint64_t map_column(map_store_t* map, int x, int y)
{
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    if (map->size_z == 64 && sizeof(size_t) == 8) {
        return (uint64_t) map->vxl.geometry[(size_t) x + (size_t) y * map->size_x];
    }
    uint64_t w = 0;
    for (int z = 0; z < map->size_z && z < 64; z++) {
        w |= (uint64_t) libvxl_map_issolid(&map->vxl, x, y, z) << z;
    }
    return w;
}

static inline int map_solid(map_store_t* map, int x, int y, int z)
{
    if (!map_inside(map, x, y, z)) {
        return 0;
    }
    return libvxl_map_issolid(&map->vxl, x, y, z);
}

/* the column's smallest solid z (its top surface), 0 when it has none */
static inline int map_top(map_store_t* map, int x, int y)
{
    uint64_t column = map_column(map, x, y);
    return column ? __builtin_ctzll(column) : 0;
}

static inline uint32_t map_color(map_store_t* map, int x, int y, int z)
{
    if (!map_inside(map, x, y, z)) {
        return 0;
    }
    if (!libvxl_map_issolid(&map->vxl, x, y, z)) {
        return MAP_STORE_DEFAULT_COLOR;
    }
    return (libvxl_map_get(&map->vxl, x, y, z) & 0x00FFFFFFu) | 0xFF000000u;
}

/* the cell solid, in that colour */
static inline void map_set_color(map_store_t* map, int x, int y, int z, uint32_t color)
{
    if (map_inside(map, x, y, z)) {
        libvxl_map_set(&map->vxl, x, y, z, color & 0x00FFFFFFu);
        map->version++;
    }
}

static inline void map_set_air(map_store_t* map, int x, int y, int z)
{
    if (map_inside(map, x, y, z)) {
        libvxl_map_setair(&map->vxl, x, y, z);
        map->version++;
    }
}

/* the map's shape hash (common/maphash: every column's solid cells) into
   hex_out, 65 bytes; the server list finds a map by it */
static inline void map_store_shape(map_store_t* map, char hex_out[65])
{
    maphash_sha256_t ctx;
    maphash_sha256_init(&ctx);
    for (int y = 0; y < map->size_y; y++) {
        for (int x = 0; x < map->size_x; x++) {
            maphash_column(&ctx, map_column(map, x, y));
        }
    }
    maphash_hex(&ctx, hex_out);
}

#endif
