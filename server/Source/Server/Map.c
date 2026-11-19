// Copyright DarkNeutrino 2021
#include <Server/BotPlanner.h>
#include <Server/EventLog.h>
#include <Server/MapPreview.h>
#include <Server/Proto10/Session.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Compress.h>
#include <Util/DataStream.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Queue.h>
#include <Util/Types.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>
#include <Util/DataDir.h>
#include <errno.h>
#include <Server/MapStore.h>
#include <ngmap/ngmap.h>
#include <Server/Profile.h>
#include <sys/stat.h>
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <Server/BlockHealth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the file's bytes (malloc'ed) and their size; NULL when unreadable */
static uint8_t* _file_bytes(const char* path, size_t* size)
{
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0L, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    uint8_t* data = n > 0 ? (uint8_t*) malloc((size_t) n) : NULL;
    if (data != NULL && fread(data, 1, (size_t) n, fp) != (size_t) n) {
        free(data);
        data = NULL;
    }
    fclose(fp);
    *size = data != NULL ? (size_t) n : 0;
    return data;
}

/* the map's ngmap next to its VXL (<Name>.ngmap for <Name>.vxl) decoded
   straight into the map: its stream, kept for every 1.0 join, and
   its size in *stream_size. NULL when there is none, or it is not a map of
   the toml's size (the VXL is read then) */
static uint8_t* _from_ngmap(server_t* server, const char* vxl_path, size_t* stream_size)
{
    char   path[1200];
    size_t n = strlen(vxl_path);
    if (n < 4 || strcmp(vxl_path + n - 4, ".vxl") != 0 || n + 3 >= sizeof path) {
        return NULL;
    }
    snprintf(path, sizeof path, "%.*s.ngmap", (int) (n - 4), vxl_path);
    size_t   size;
    uint8_t* data = _file_bytes(path, &size);
    if (data == NULL) {
        return NULL;
    }
    int err = map_store_read_ngmap(&server->s_map.map, data, size);
    if (err != NGMAP_OK) {
        LOG_WARNING("Map %s: its ngmap is %s, the VXL is read instead",
                    server->map_name,
                    err == NGMAP_ERR_ARGS ? "not the toml's size" : ngmap_strerror(err));
        free(data);
        return NULL;
    }
    *stream_size = size;
    return data;
}

uint8_t map_load(server_t* server, const char* path, int map_size[3])
{
    LOG_STATUS("Loading map");
    /* ngmap encodings of the previous map must not reach anyone */
    proto10_session_map_loaded();

    if (map_store_live(&server->s_map.map)) {
        map_store_free(&server->s_map.map);
    }
    /* the previous map's shape and picture must not answer for a map that
       fails to load */
    server->map_shape[0] = '\0';
    map_preview_free(server);
    map_store_create(&server->s_map.map, map_size[0], map_size[1], map_size[2]);
    server->s_map.map_size = 0;

    /* protocol 1.0 sends the map's ngmap as it is: a map that has one
       next to its VXL is loaded from it, so the server plays the very map
       its clients receive. Decoded into the map itself: the VXL bytes it
       went through were the load's peak (docs/map-memory.md) */
    size_t   stream_size = 0;
    uint8_t* stream      = profile_is_075() ? NULL : _from_ngmap(server, path, &stream_size);
    char     save[1200]  = "";
    int      readable    = 1;
    if (stream == NULL) {
        size_t   vxl_size;
        uint8_t* buffer = _file_bytes(path, &vxl_size);
        if (buffer == NULL) {
            LOG_ERROR("Unable to open map at path %s with error: %s", path, strerror(errno));
            exit(EXIT_FAILURE);
        }
        server->s_map.map_size = vxl_size;
        if (vxl_size > map_store_write_bound(&server->s_map.map)) {
            LOG_ERROR("Map file %s.vxl is larger then maximum VXL size of X: %d, Y: %d, Z: %d. Please set the "
                      "correct map size in its toml",
                      server->map_name,
                      server->s_map.map.size_x,
                      server->s_map.map.size_y,
                      server->s_map.map.size_z);
            map_store_free(&server->s_map.map);
            free(buffer);
            server->running = 0;
            return 0;
        }
        LOG_STATUS("Transforming map from VXL");
        readable = map_store_read(&server->s_map.map, buffer, vxl_size);
        if (!readable) {
            /* MapDownload checks a downloaded VXL's spans; a file copied in
               by hand is checked here, and is no map rather than half of one */
            LOG_ERROR("Map %s: the VXL is not a %dx%dx%d map", server->map_name, map_size[0], map_size[1],
                      map_size[2]);
            evt_log(NULL, "map", "action=refused why=vxl name=%s", server->map_name);
        }
        /* the stream a 1.0 join gets: this VXL's encoding kept from an
           earlier load (data folder's ngmap/, by the VXL's CRC32), else it is
           encoded now and kept there */
        if (!profile_is_075()) {
            char     dir[1100], name[64];
            uint32_t crc = (uint32_t) crc32(crc32(0L, Z_NULL, 0), buffer, (uInt) vxl_size);
            data_path(dir, sizeof dir, "ngmap");
#ifdef _WIN32
            _mkdir(dir);
#else
            mkdir(dir, 0755);
#endif
            snprintf(name, sizeof name, "/%08x-%zu.ngmap", crc, vxl_size);
            snprintf(save, sizeof save, "%s%s", dir, name);
            stream = _file_bytes(save, &stream_size);
        }
        free(buffer);
    }
    LOG_STATUS("Finished loading map");

    /* the shape and the top view from the map as loaded, before a match
       digs it: the server list looks a map up by its shape */
    uint64_t shape_at = get_real_nanos();
    if (readable) {
        map_store_shape(&server->s_map.map, server->map_shape);
        LOG_STATUS("Map shape: %s", server->map_shape);
        evt_log(NULL, "map", "action=shape hash=%s ms=%.3f", server->map_shape,
                (double) (get_real_nanos() - shape_at) / 1e6);
        map_preview_make(server, &server->s_map.map);
    } else {
        /* an empty map's shape would name another map */
        evt_log(NULL, "map", "action=shape_refused why=vxl ms=%.3f", (double) (get_real_nanos() - shape_at) / 1e6);
    }

    if (stream != NULL) {
        proto10_session_map_stored(server, stream, stream_size, save[0] ? "kept" : "map");
        free(stream);
    }
    /* the bots' planner works on a copy of its own */
    planner_map_loaded(server);
    block_health_reset();
    proto10_session_map_ready(server, save[0] ? save : NULL);
    return 1;
}

static char map_user_dir[512];

void map_set_user_dir(const char* dir)
{
    snprintf(map_user_dir, sizeof(map_user_dir), "%s", dir != NULL ? dir : "");
}

/* the folder downloads should prefer: MapDownload.c puts a new map there
   when it is writable (a listen server keeps every map of its own in it) */
const char* map_user_dir_get(void)
{
    return map_user_dir;
}

/* <base>/<name>/<name>.<ext> into out when that folder's .vxl is there */
static int _map_in(char* out, size_t size, const char* base, const char* name, const char* ext)
{
    snprintf(out, size, "%s/%s/%s.vxl", base, name, name);
    FILE* fp = fopen(out, "rb");
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    snprintf(out, size, "%s/%s/%s.%s", base, name, name, ext);
    return 1;
}

/* the folder holding <name>.ngmap and <name>.toml but no .vxl yet (a map
   folder copied from the maps repository, which keeps only ngmap), in
   map_file_path's order */
int map_ngmap_folder(char* out, size_t size, const char* name)
{
    char downloaded[1100];
    data_path(downloaded, sizeof downloaded, "maps");
    const char* bases[] = {map_user_dir, "Resources/maps", downloaded};
    for (int k = 0; k < 3; k++) {
        if (bases[k][0] == '\0') {
            continue;
        }
        char  path[1400];
        FILE* fp;
        snprintf(path, sizeof path, "%s/%s/%s.ngmap", bases[k], name, name);
        if ((fp = fopen(path, "rb")) == NULL) {
            continue;
        }
        fclose(fp);
        snprintf(path, sizeof path, "%s/%s/%s.toml", bases[k], name, name);
        if ((fp = fopen(path, "rb")) == NULL) {
            continue;
        }
        fclose(fp);
        snprintf(out, size, "%s/%s", bases[k], name);
        return 1;
    }
    return 0;
}

void map_file_path(char* out, size_t size, const char* name, const char* ext)
{
    if (map_user_dir[0] != '\0' && _map_in(out, size, map_user_dir, name, ext)) {
        return;
    }
    if (_map_in(out, size, "Resources/maps", name, ext)) {
        return;
    }
    /* where MapDownload.c puts a map when Resources/maps is read only */
    char downloaded[1100];
    data_path(downloaded, sizeof downloaded, "maps");
    if (strcmp(downloaded, "Resources/maps") != 0 && _map_in(out, size, downloaded, name, ext)) {
        return;
    }
    snprintf(out, size, "Resources/maps/%s/%s.%s", name, name, ext);
}
