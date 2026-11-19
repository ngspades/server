// Copyright CircumScriptor and DarkNeutrino 2021
#include <Server/Structs/ServerStruct.h>
#include <Util/Compress.h>
#include <Util/Log.h>
#include <Util/Queue.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>
#include <Server/MapStore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static z_stream* g_compressor = NULL;

/**
 * @brief Initialize compressor library (global compressor)
 *
 * @param level Level of compression
 * @return 0 on success
 */
static int _compress_init(server_t* server, int level)
{
    (void) server;

    if (g_compressor != NULL) {
        LOG_ERROR("Compressor is already initialized");
        return 1;
    } else {
        g_compressor = (z_stream*) spadesx_malloc(sizeof(z_stream));
    }

    g_compressor->zalloc = Z_NULL;
    g_compressor->zfree  = Z_NULL;
    g_compressor->opaque = Z_NULL;

    if (deflateInit(g_compressor, level) < 0) {
        LOG_ERROR("Failed to initialize compressor");
        return 1;
    }
    return 0;
}

/**
 * @brief Close compressor library (global compressor)
 *
 * @return 0 on success
 */
static int _compress_close(void)
{
    if (g_compressor == NULL) {
        LOG_ERROR("Compressor is not initialized");
        return 1;
    }

    if (deflateEnd(g_compressor) != 0) {
        LOG_ERROR("Failed to close compressor");
        return 1;
    }

    free(g_compressor);
    g_compressor = NULL;
    return 0;
}

queue_t* compress_queue(server_t* server, uint8_t* data, uint32_t length, uint32_t chunkSize)
{
    _compress_init(server, 5);
    if (g_compressor == NULL) {
        LOG_ERROR("Compressor is not initialized");
        return NULL;
    }

    queue_t* parent = NULL;
    queue_t* node;

    g_compressor->next_in  = (uint8_t*) data;
    g_compressor->avail_in = length;

    do {
        node        = (queue_t*) spadesx_malloc(sizeof(*node));
        node->block = (uint8_t*) spadesx_malloc(chunkSize);
        g_compressor->avail_out = chunkSize;
        g_compressor->next_out  = node->block;
        if (deflate(g_compressor, Z_FINISH) < 0) {
            LOG_ERROR("Failed to compress data");
            return NULL;
        }
        node->length = chunkSize - g_compressor->avail_out;
        DL_APPEND(parent, node);
    } while (g_compressor->avail_out == 0);
    _compress_close();
    return parent;
}

/* a node of chunk_size bytes appended to *queue, deflate writing into it */
static int _next_node(z_stream* zs, queue_t** queue, uint32_t chunk_size)
{
    queue_t* node = (queue_t*) malloc(sizeof *node);
    if (node == NULL || (node->block = (uint8_t*) malloc(chunk_size)) == NULL) {
        free(node);
        return 0;
    }
    node->capacity = chunk_size;
    node->length   = 0;
    DL_APPEND(*queue, node);
    zs->next_out  = node->block;
    zs->avail_out = chunk_size;
    return 1;
}

void compress_queue_free(queue_t** queue)
{
    queue_t *node, *tmp;
    DL_FOREACH_SAFE(*queue, node, tmp)
    {
        DL_DELETE(*queue, node);
        free(node->block);
        free(node);
    }
}

queue_t* compress_map(map_store_t* map, uint32_t chunk_size, size_t* vxl_bytes)
{
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit(&zs, 5) != Z_OK) {
        return NULL;
    }
    queue_t*        queue = NULL;
    struct vxlwrite w;
    uint8_t*        in    = (uint8_t*) malloc(COMPRESS_MAP_SLICE);
    int             ok    = in != NULL && vxlwrite_begin(&w, &map->vxl) && _next_node(&zs, &queue, chunk_size);
    size_t          bytes = 0;
    int             flush = Z_NO_FLUSH;
    while (ok && flush != Z_FINISH) {
        /* a slice of columns, then deflate takes all of it */
        size_t used = 0;
        while (!vxlwrite_done(&w) && COMPRESS_MAP_SLICE - used >= VXLWRITE_COLUMN_MAX(map->size_z)) {
            used += vxlwrite_column(&w, in + used);
        }
        bytes += used;
        flush       = vxlwrite_done(&w) ? Z_FINISH : Z_NO_FLUSH;
        zs.next_in  = in;
        zs.avail_in = (uInt) used;
        for (;;) {
            int ret             = deflate(&zs, flush);
            queue->prev->length = queue->prev->capacity - zs.avail_out;
            if (ret == Z_STREAM_ERROR) {
                ok = 0;
                break;
            }
            if (ret == Z_STREAM_END) {
                break;
            }
            if (zs.avail_out == 0) {
                if (!_next_node(&zs, &queue, chunk_size)) {
                    ok = 0;
                    break;
                }
                continue;
            }
            /* room left over: deflate took the whole slice (and with
               Z_FINISH it would have ended the stream) */
            ok = flush != Z_FINISH;
            break;
        }
    }
    if (in != NULL && w.cursor != NULL) {
        vxlwrite_end(&w);
    }
    free(in);
    deflateEnd(&zs);
    if (!ok) {
        compress_queue_free(&queue);
        return NULL;
    }
    /* deflate may end exactly at a node's end: no empty node to send */
    if (queue != NULL && queue->prev->length == 0) {
        queue_t* last = queue->prev;
        DL_DELETE(queue, last);
        free(last->block);
        free(last);
    }
    if (vxl_bytes != NULL) {
        *vxl_bytes = bytes;
    }
    return queue;
}
