/*
 * ngmap: C API for the map format of OpenSpades' 2014 "ngspades" branch.
 *
 * The codec converts between AoS VXL bytes and ngmap streams so the client and
 * the server keep their own VXL loaders. Protocol 1.0 (docs/protocol-1.0.md,
 * section 7.5) sends maps in this format.
 */
#ifndef NGMAP_H
#define NGMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest accepted map; anything bigger is rejected before allocating. */
#define NGMAP_MAX_WIDTH 2048
#define NGMAP_MAX_HEIGHT 2048
#define NGMAP_MAX_DEPTH 64

enum {
    NGMAP_OK = 0,
    NGMAP_ERR_ARGS = 1,      /* bad argument (null pointer, quality, size) */
    NGMAP_ERR_VXL = 2,       /* the VXL input is malformed */
    NGMAP_ERR_CORRUPT = 3,   /* the ngmap stream is malformed or truncated */
    NGMAP_ERR_TOO_LARGE = 4, /* dimensions or voxel count above the limits */
    NGMAP_ERR_NOMEM = 5,
    NGMAP_ERR_INTERNAL = 6
};

/*
 * Encodes a VXL map. quality is 1..100 (100 keeps colours within 1 step per
 * channel; lower values use the JPEG tiles and coarser colour models).
 * On success *out is allocated with malloc() and the caller frees it.
 */
int ngmap_encode(const uint8_t* vxl, size_t vxl_size, int width, int height, int depth, int quality,
                 uint8_t** out, size_t* out_size);

/*
 * Decodes an ngmap stream into VXL bytes (malloc()ed, caller frees) and the
 * map dimensions. Safe on untrusted input: malformed data returns an error.
 * Visible voxels carry the decoded colour; any other voxel VXL needs a colour
 * for gets libvxl's default brown.
 */
int ngmap_decode(const uint8_t* data, size_t size, uint8_t** vxl_out, size_t* vxl_size, int* width,
                 int* height, int* depth);

/*
 * Decodes an ngmap stream column by column, with no VXL in between: the
 * callers build their own map from it (common/ngmap/ngmap_libvxl.h builds
 * libvxl's). size(user, w, h, d) is called once, after the whole stream
 * decoded; a nonzero return refuses the map (NGMAP_ERR_ARGS) and no column
 * follows. Then column(user, x, y, solid, colored, colors) for every column
 * in VXL order (y, then x): bit z of solid is voxel z solid, bit z of colored
 * a solid voxel a VXL file gives a colour (the stored ones and the top and
 * bottom of every solid run, exactly what ngmap_decode writes), colors one
 * per bit of colored from the lowest z, as the VXL bytes hold it read as a
 * little-endian u32 (0x7FRRGGBB, the default brown where none is stored).
 */
typedef int (*ngmap_size_fn)(void* user, int width, int height, int depth);
typedef void (*ngmap_column_fn)(void* user, int x, int y, uint64_t solid, uint64_t colored, const uint32_t* colors);
int ngmap_decode_columns(const uint8_t* data, size_t size, ngmap_size_fn on_size, ngmap_column_fn on_column, void* user);

/* Human readable name of an error code. */
const char* ngmap_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
