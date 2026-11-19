// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/MapPreview.h>
#include <Server/MapStore.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* 256 pixels an axis: a 512 column map becomes one pixel a 2 x 2 columns,
   the detail of Jagex's "Choose Game" preview. Each channel keeps its top
   5 bits (PREVIEW_MASK): no eye tells 32 shades from 256 on a minimap, and
   zlib then packs Raindance into 44 KB instead of 143 (128 pixels with
   every bit was 37 KB), about a second at the query's 40 chunks a second */
#define PREVIEW_SIZE 256
#define PREVIEW_MASK 0xF8

/* The colour of the top visible block of a column at z, shaded by its
   height so the relief reads (0 is the sky). The water layer (the bottom
   one, z 63 on a 64 deep map) keeps its own colour: shading it only made
   the sea black. */
static void _preview_shade(uint8_t out[3], const uint8_t* colour, int z, int depth)
{
    float f = 1.0f;
    if (z < depth - 1) {
        f = 0.55f + 0.45f * (1.0f - (float) z / (float) (depth - 1));
    }
    /* AoS VXL stores B, G, R, A */
    out[0] = (uint8_t) ((float) colour[2] * f) & PREVIEW_MASK;
    out[1] = (uint8_t) ((float) colour[1] * f) & PREVIEW_MASK;
    out[2] = (uint8_t) ((float) colour[0] * f) & PREVIEW_MASK;
}

void map_preview_free(server_t* server)
{
    free(server->map_preview);
    server->map_preview     = NULL;
    server->map_preview_len = 0;
}

void map_preview_make(server_t* server, map_store_t* map)
{
    map_preview_free(server);
    int w = map->size_x, h = map->size_y, d = map->size_z;
    if (!map_store_live(map) || w <= 0 || h <= 0 || d <= 0 || d > 64) {
        return;
    }
    uint64_t at = get_real_nanos();

    /* which column of the map each pixel samples: the middle of its cell */
    int* px_of_col = (int*) malloc((size_t) w * sizeof(int));
    int* py_of_row = (int*) malloc((size_t) h * sizeof(int));
    uint8_t* rgb   = (uint8_t*) calloc((size_t) PREVIEW_SIZE * PREVIEW_SIZE, 3);
    if (px_of_col == NULL || py_of_row == NULL || rgb == NULL) {
        free(px_of_col);
        free(py_of_row);
        free(rgb);
        evt_log(NULL, "map", "action=preview_failed why=memory ms=%.3f",
                (double) (get_real_nanos() - at) / 1e6);
        return;
    }
    for (int i = 0; i < w; i++) {
        px_of_col[i] = -1;
    }
    for (int i = 0; i < h; i++) {
        py_of_row[i] = -1;
    }
    for (int i = 0; i < PREVIEW_SIZE; i++) {
        int x = (int) (((double) i + 0.5) * (double) w / (double) PREVIEW_SIZE);
        int y = (int) (((double) i + 0.5) * (double) h / (double) PREVIEW_SIZE);
        if (x >= w) {
            x = w - 1;
        }
        if (y >= h) {
            y = h - 1;
        }
        px_of_col[x] = i;
        py_of_row[y] = i;
    }

    /* each sampled column's top solid cell, in its colour: the first colour
       of the VXL's first span, which the map keeps for every surface cell */
    for (int y = 0; y < h; y++) {
        int py = py_of_row[y];
        if (py < 0) {
            continue;
        }
        for (int x = 0; x < w; x++) {
            int px = px_of_col[x];
            if (px < 0 || map_column(map, x, y) == 0) {
                continue;
            }
            int      top    = map_top(map, x, y);
            uint32_t c      = map_color(map, x, y, top);
            uint8_t  bgr[3] = {(uint8_t) c, (uint8_t) (c >> 8), (uint8_t) (c >> 16)};
            _preview_shade(rgb + 3 * ((size_t) py * PREVIEW_SIZE + (size_t) px), bgr, top, d);
        }
    }
    free(px_of_col);
    free(py_of_row);

    {
        uLongf   raw_len = (uLongf) PREVIEW_SIZE * PREVIEW_SIZE * 3;
        uLongf   zlen    = compressBound(raw_len);
        uint8_t* out     = (uint8_t*) malloc(8 + zlen);
        if (out != NULL) {
            memcpy(out, "SDP1", 4);
            out[4] = (uint8_t) (PREVIEW_SIZE & 0xFF);
            out[5] = (uint8_t) (PREVIEW_SIZE >> 8);
            out[6] = (uint8_t) (PREVIEW_SIZE & 0xFF);
            out[7] = (uint8_t) (PREVIEW_SIZE >> 8);
            if (compress2(out + 8, &zlen, rgb, raw_len, 9) != Z_OK) {
                free(out);
                out = NULL;
            } else {
                server->map_preview     = out;
                server->map_preview_len = 8 + (size_t) zlen;
            }
        }
    }
    free(rgb);

    if (server->map_preview != NULL) {
        evt_log(NULL, "map", "action=preview bytes=%zu ms=%.3f", server->map_preview_len,
                (double) (get_real_nanos() - at) / 1e6);
    } else {
        evt_log(NULL, "map", "action=preview_failed why=%s ms=%.3f", "compress",
                (double) (get_real_nanos() - at) / 1e6);
    }
}
