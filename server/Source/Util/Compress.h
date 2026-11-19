// Copyright CircumScriptor and DarkNeutrino 2021
#ifndef COMPRESS_H
#define COMPRESS_H

#include <Server/Structs/ServerStruct.h>
#include <Server/MapStore.h>
#include <Util/Queue.h>
#include <Util/Types.h>

#ifndef DEFAULT_COMPRESS_CHUNK_SIZE
    #define DEFAULT_COMPRESS_CHUNK_SIZE 8192
#endif /* DEFAULT_COMPRESS_CHUNK_SIZE */

/**
 * @brief Compress data (using global compressor)
 *
 * @param data Data to be compressed
 * @param length Length of data in bytes
 * @param chunkSize Size of one block of compressed data
 * @return Blocks of compressed data (queue) or null
 */
queue_t* compress_queue(server_t* server, uint8_t* data, uint32_t length, uint32_t chunkSize);

/* the map as zlib VXL in blocks of chunk_size bytes (the 0.75 MapChunks, a
   1.0 VXL transfer), written a slice of columns at a time into deflate
   (common/vxlwrite): no buffer of the whole VXL (4 to 10 MB a join at 512,
   and its bound reserved 135 MB). NULL out of memory; *vxl_bytes the VXL's
   size */
#define COMPRESS_MAP_SLICE (64 * 1024)
queue_t* compress_map(map_store_t* map, uint32_t chunk_size, size_t* vxl_bytes);
/* every node of the queue, and the queue NULL */
void compress_queue_free(queue_t** queue);

#endif // COMPRESS_H
