// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Map.h>
#include <Server/MapDownload.h>
#include <Server/PluginNet.h>
#include <Util/DataDir.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <ngmap/ngmap.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <json-c/json.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif
#ifdef ENABLE_CURL
#include <curl/curl.h>
#endif

/* the only place maps come from: never a host a config could name. One
   constant for the repository's root, so the maps and aliases.json come from
   the same place; SP_MAPS_URL replaces it for a test that serves a folder of
   its own (see _base) */
#define MAPS_BASE_DEFAULT "https://raw.githubusercontent.com/ngspades/maps/main/"
/* map_load refuses more than 512 x 512 x 32 x 8 bytes; the repository's
   largest is ~12 MB (an ngmap is a seventh of it) */
#define MAP_VXL_MAX  (64L * 1024 * 1024)
#define MAP_TEXT_MAX (1024L * 1024)
/* tools/map_aliases.py writes it; every name of every map, so a few hundred
   KB, and 4 MB is already far more than it can be */
#define MAP_ALIASES_FILE "maps/aliases.json"
#define MAP_ALIASES_MAX  (4L * 1024 * 1024)
/* a day: the file changes only when a map is added or renamed */
#define MAP_ALIASES_AGE  (24L * 60 * 60)

static int             dl_enabled = 1;
static pthread_mutex_t dl_lock    = PTHREAD_MUTEX_INITIALIZER;
static char            dl_pending[MAP_NAME_MAX + 1];
static char            maps_base[600];
static int             maps_base_set;
static int             maps_plain_http;

/* the repository's root, with a trailing slash. SP_MAPS_URL is a test's: it
   may serve a copy of the repository from this machine, so it is taken only
   as https or as plain http on 127.0.0.1 (any other host would let a config
   or the environment point the server at a stranger) */
static const char* _base(void)
{
    if (maps_base_set) {
        return maps_base;
    }
    maps_base_set = 1;
    snprintf(maps_base, sizeof maps_base, "%s", MAPS_BASE_DEFAULT);
    const char* env = getenv("SP_MAPS_URL");
    if (env == NULL || *env == '\0') {
        return maps_base;
    }
    if (!strncmp(env, "https://", 8)) {
        maps_plain_http = 0;
    } else if (!strncmp(env, "http://127.0.0.1", 16) &&
               (env[16] == '\0' || env[16] == ':' || env[16] == '/'))
    {
        maps_plain_http = 1;
    } else {
        LOG_WARNING("SP_MAPS_URL '%s' ignored: https, or http on 127.0.0.1", env);
        return maps_base;
    }
    size_t n = strlen(env);
    snprintf(maps_base, sizeof maps_base, "%s%s", env, env[n - 1] == '/' ? "" : "/");
    return maps_base;
}

void map_download_configure(toml_table_t* root)
{
    dl_enabled         = 1;
    toml_table_t* maps = root != NULL ? toml_table_in(root, "maps") : NULL;
    if (maps != NULL) {
        toml_datum_t d = toml_bool_in(maps, "download");
        if (d.ok) {
            dl_enabled = d.u.b ? 1 : 0;
        }
    }
    const char* env = getenv("SP_MAP_DOWNLOAD");
    if (env != NULL && *env != '\0') {
        dl_enabled = atoi(env) != 0;
    }
}

int map_download_enabled(void)
{
    return dl_enabled;
}

int map_name_ok(const char* name)
{
    size_t n = name != NULL ? strlen(name) : 0;
    if (n == 0 || n > MAP_NAME_MAX || name[0] == '.') {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
        {
            return 0;
        }
    }
    return 1;
}

static int _exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int map_installed(const char* name)
{
    char path[512];
    map_file_path(path, sizeof path, name, "vxl");
    if (!_exists(path)) {
        return 0;
    }
    map_file_path(path, sizeof path, name, "toml");
    return _exists(path);
}

static int _mkdir1(const char* path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

/* <base>/<name> made and writable (a probe file created and removed) */
static int _writable_dir(const char* base, const char* name, char* out, size_t size)
{
    if (_mkdir1(base) != 0) {
        return 0;
    }
    snprintf(out, size, "%s/%s", base, name);
    if (_mkdir1(out) != 0) {
        return 0;
    }
    char probe[600];
    snprintf(probe, sizeof probe, "%s/.write-test", out);
    FILE* fp = fopen(probe, "wb");
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    remove(probe);
    return 1;
}

/* the map folder given with --maps-dir first (the folder a listen server
   passes is where its maps live, so a download belongs there too), then
   Resources/maps/<name>, else the data folder's maps/<name> */
static int _target_dir(const char* name, char* out, size_t size)
{
    if (map_user_dir_get()[0] != '\0' && _writable_dir(map_user_dir_get(), name, out, size)) {
        return 1;
    }
    if (_writable_dir("Resources/maps", name, out, size)) {
        return 1;
    }
    char base[1100];
    data_path(base, sizeof base, "maps");
    return _writable_dir(base, name, out, size);
}

typedef struct
{
    FILE* fp;
    long  written;
    long  max;
    int   too_big;
} dl_sink_t;

#ifdef ENABLE_CURL
static size_t _on_body(char* ptr, size_t size, size_t nmemb, void* user)
{
    dl_sink_t* s = user;
    size_t     n = size * nmemb;
    if (s->written + (long) n > s->max) {
        s->too_big = 1;
        return 0;
    }
    if (fwrite(ptr, 1, n, s->fp) != n) {
        return 0;
    }
    s->written += (long) n;
    return n;
}
#endif

/* one file into path; 1 done, 0 not found (404), -1 failed (why in why) */
static int _fetch(const char* url, const char* path, long max, long* bytes, char* why, size_t why_size)
{
#ifdef ENABLE_CURL
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) {
        snprintf(why, why_size, "cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        fclose(fp);
        snprintf(why, why_size, "curl_easy_init failed");
        return -1;
    }
    dl_sink_t sink = {fp, 0, max, 0};
    char      agent[64];
    snprintf(agent, sizeof agent, "%s", GAME_SERVER_TITLE);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t) max);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, maps_plain_http ? "http" : "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, maps_plain_http ? (long) CURLPROTO_HTTP : (long) CURLPROTO_HTTPS);
#endif
    net_curl_tls(curl);
    CURLcode res    = curl_easy_perform(curl);
    long     status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (fclose(fp) != 0 && res == CURLE_OK) {
        snprintf(why, why_size, "cannot write %s", path);
        remove(path);
        return -1;
    }
    *bytes = sink.written;
    if (res != CURLE_OK) {
        remove(path);
        snprintf(why, why_size, "%s", sink.too_big || res == CURLE_FILESIZE_EXCEEDED ? "too large" : curl_easy_strerror(res));
        return -1;
    }
    if (status == 404) {
        remove(path);
        return 0;
    }
    if (status != 200) {
        remove(path);
        snprintf(why, why_size, "HTTP %ld", status);
        return -1;
    }
    return 1;
#else
    (void) url;
    (void) path;
    (void) max;
    (void) bytes;
    (void) maps_plain_http; /* only libcurl's protocol list reads it */
    snprintf(why, why_size, "built without libcurl");
    return -1;
#endif
}

/* the toml the server will read: it parses, and its [map] and [spawnpoints]
   are there (Server.c exits without them). Its map_size into size */
static int _check_toml(const char* path, int size[3], char* why, size_t why_size)
{
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        snprintf(why, why_size, "cannot read the toml");
        return 0;
    }
    char          err[200];
    toml_table_t* root = toml_parse_file(fp, err, sizeof err);
    fclose(fp);
    if (root == NULL) {
        snprintf(why, why_size, "toml: %s", err);
        return 0;
    }
    toml_table_t* map = toml_table_in(root, "map");
    int           ok  = map != NULL && toml_table_in(root, "spawnpoints") != NULL;
    size[0] = 512;
    size[1] = 512;
    size[2] = 64;
    toml_array_t* a = map != NULL ? toml_array_in(map, "map_size") : NULL;
    for (int k = 0; a != NULL && k < 3; k++) {
        toml_datum_t d = toml_int_at(a, k);
        if (d.ok) {
            size[k] = (int) d.u.i;
        }
    }
    toml_free(root);
    if (!ok) {
        snprintf(why, why_size, "toml without [map] or [spawnpoints]");
        return 0;
    }
    if (size[0] < 1 || size[0] > 4096 || size[1] < 1 || size[1] > 4096 || size[2] < 1 || size[2] > 256) {
        snprintf(why, why_size, "map_size %d x %d x %d", size[0], size[1], size[2]);
        return 0;
    }
    return 1;
}

/* the VXL's spans walked column by column: every column ends inside the
   file, no span leaves the map's height, and the last column ends the file.
   libmapvxl trusts its input; a truncated or foreign file must not reach it */
static int _check_vxl(const char* path, const int size[3], char* why, size_t why_size)
{
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(why, why_size, "cannot read the vxl");
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > MAP_VXL_MAX) {
        fclose(fp);
        snprintf(why, why_size, "vxl of %ld bytes", len);
        return 0;
    }
    uint8_t* v = malloc((size_t) len);
    if (v == NULL || fread(v, 1, (size_t) len, fp) != (size_t) len) {
        fclose(fp);
        free(v);
        snprintf(why, why_size, "cannot read the vxl");
        return 0;
    }
    fclose(fp);
    long   pos     = 0;
    long   columns = (long) size[0] * size[1];
    int    ok      = 1;
    for (long c = 0; c < columns && ok; c++) {
        for (;;) {
            if (pos + 4 > len) {
                ok = 0;
                break;
            }
            int n = v[pos], s = v[pos + 1], e = v[pos + 2];
            if (s >= size[2] || e >= size[2] || e + 1 < s) {
                ok = 0;
                break;
            }
            if (n == 0) {
                pos += 4L * (e - s + 2);
                break;
            }
            pos += 4L * n;
        }
        if (pos > len) {
            ok = 0;
        }
    }
    free(v);
    if (!ok || pos != len) {
        snprintf(why, why_size, "not a %d x %d x %d VXL", size[0], size[1], size[2]);
        return 0;
    }
    return 1;
}

static uint8_t* _read_all(const char* path, long max, long* len)
{
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    *len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* data = *len > 0 && *len <= max ? malloc((size_t) *len) : NULL;
    if (data != NULL && fread(data, 1, (size_t) *len, fp) != (size_t) *len) {
        free(data);
        data = NULL;
    }
    fclose(fp);
    return data;
}

/* an ngmap (the maps repository holds nothing else) decoded into the VXL
   the server loads, written to vxl_path. The decoder is safe on untrusted
   input; the dimensions must be the toml's, and the VXL still goes through
   _check_vxl like a downloaded one */
static int _ngmap_to_vxl(const char* ngmap_path, const char* vxl_path, const int size[3], char* why,
                         size_t why_size)
{
    long     len  = 0;
    uint8_t* data = _read_all(ngmap_path, MAP_VXL_MAX, &len);
    if (data == NULL) {
        snprintf(why, why_size, "cannot read the ngmap");
        return 0;
    }
    uint8_t* vxl      = NULL;
    size_t   vxl_size = 0;
    int      w = 0, h = 0, d = 0;
    int      err = ngmap_decode(data, (size_t) len, &vxl, &vxl_size, &w, &h, &d);
    free(data);
    if (err != 0) {
        snprintf(why, why_size, "ngmap: %s", ngmap_strerror(err));
        return 0;
    }
    if (w != size[0] || h != size[1] || d != size[2]) {
        free(vxl);
        snprintf(why, why_size, "ngmap of %d x %d x %d, the toml says %d x %d x %d", w, h, d, size[0], size[1],
                 size[2]);
        return 0;
    }
    FILE* fp = fopen(vxl_path, "wb");
    int   ok = fp != NULL && fwrite(vxl, 1, vxl_size, fp) == vxl_size;
    if (fp != NULL && fclose(fp) != 0) {
        ok = 0;
    }
    free(vxl);
    if (!ok) {
        snprintf(why, why_size, "cannot write %s", vxl_path);
        remove(vxl_path);
        return 0;
    }
    return 1;
}

/* a failed download leaves nothing: its parts, and its folder when empty
   (a folder without its .vxl is no map, but an empty one misleads a host) */
static void _remove3(const char* dir, const char* name)
{
    const char* ext[] = {"vxl", "toml", "txt", "ngmap"};
    char        path[700];
    for (int k = 0; k < 4; k++) {
        snprintf(path, sizeof path, "%s/%s.%s.part", dir, name, ext[k]);
        remove(path);
    }
#ifdef _WIN32
    _rmdir(dir);
#else
    rmdir(dir); /* fails, as it should, when the folder holds anything */
#endif
}

static int _move(const char* from, const char* to)
{
#ifdef _WIN32
    remove(to); /* Windows' rename does not replace */
#endif
    /* elsewhere rename replaces in one step: another server starting from
       the same folder never sees the map missing */
    return rename(from, to);
}

/* this process's number, for the name of a file being written: two servers
   unpacking one map at once (instances started together from one folder)
   wrote the same .vxl.part, one truncating it while the other checked it
   ("vxl of 0 bytes"), and that server did not start */
static long _pid(void)
{
#ifdef _WIN32
    return (long) GetCurrentProcessId();
#else
    return (long) getpid();
#endif
}

/* tools/maplib.py's norm(): lower case, letters and digits only. The two
   must agree, or an alias the tool wrote is never found here */
static void _norm(const char* name, char* out, size_t size)
{
    size_t o = 0;
    for (const char* p = name; *p != '\0' && o + 1 < size; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* the map is there as a folder, its VXL written or still to unpack from its
   ngmap (a folder copied from the repository: map_download_now unpacks it) */
static int _local_installed(const char* folder)
{
    char path[1400];
    return map_installed(folder) || map_ngmap_folder(path, sizeof path, folder);
}

/* the folder in `base` whose name normalises to `norm` */
static int _local_alias_in(const char* base, const char* norm, char* out, size_t size)
{
    if (base == NULL || base[0] == '\0') {
        return 0;
    }
    int found = 0;
#ifdef _WIN32
    char pattern[1400];
    snprintf(pattern, sizeof pattern, "%s/*", base);
    WIN32_FIND_DATAA entry;
    HANDLE           h = FindFirstFileA(pattern, &entry);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry.cFileName[0] == '.') {
            continue;
        }
        char key[MAP_NAME_MAX * 2];
        _norm(entry.cFileName, key, sizeof key);
        if (strcmp(key, norm) == 0 && _local_installed(entry.cFileName)) {
            snprintf(out, size, "%s", entry.cFileName);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &entry));
    FindClose(h);
#else
    DIR* dir = opendir(base);
    if (dir == NULL) {
        return 0;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char key[MAP_NAME_MAX * 2];
        _norm(entry->d_name, key, sizeof key);
        if (strcmp(key, norm) != 0 || !_local_installed(entry->d_name)) {
            continue;
        }
        snprintf(out, size, "%s", entry->d_name);
        found = 1;
        break;
    }
    closedir(dir);
#endif
    return found;
}

static int _aliases_stale(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 1;
    }
    time_t now = time(NULL);
    /* a clock that went backwards says nothing about the file's age: fetch
       it again rather than trust it for good */
    return st.st_mtime <= 0 || st.st_mtime > now || (now - st.st_mtime) >= MAP_ALIASES_AGE;
}

static int _aliases_fetch(const char* path)
{
    char url[900], part[1400], why[200] = "";
    long bytes = 0;
    snprintf(url, sizeof url, "%saliases.json", _base());
    snprintf(part, sizeof part, "%s.part", path);
    if (_fetch(url, part, MAP_ALIASES_MAX, &bytes, why, sizeof why) <= 0 || _move(part, path) != 0) {
        remove(part);
        return 0;
    }
    return 1;
}

/* one attempt a minute: a rotation of ten maps the host does not have would
   otherwise try ten times, and each try waits for a connection timeout on a
   machine that is offline */
static int _aliases_fetch_due(void)
{
    static uint64_t last;
    uint64_t        now = get_real_nanos();
    if (last != 0 && now - last < 60ULL * NANO_IN_SECOND) {
        return 0;
    }
    last = now;
    return 1;
}

/* the folder aliases.json (tools/map_aliases.py) gives a normalised name.
   may_fetch: 0 on the tick, where a download of a few hundred KB would
   stall everyone; the background download asks again off the tick */
static int _alias_from_file(const char* norm, char* out, size_t size, int may_fetch)
{
    char path[1400];
    data_path(path, sizeof path, MAP_ALIASES_FILE);
    if (may_fetch && _aliases_stale(path) && _aliases_fetch_due()) {
        char base[1100];
        data_path(base, sizeof base, "maps");
        _mkdir1(base);
        /* a failed fetch keeps the copy that is there: a day old at most
           unless the server was offline, and the names it knows still work */
        _aliases_fetch(path);
    }
    long     len  = 0;
    uint8_t* data = _read_all(path, MAP_ALIASES_MAX, &len);
    if (data == NULL) {
        return 0;
    }
    char* text = malloc((size_t) len + 1);
    int   ok   = 0;
    if (text != NULL) {
        memcpy(text, data, (size_t) len);
        text[len]          = '\0';
        json_object* root  = json_tokener_parse(text);
        json_object* value = NULL;
        if (root != NULL && json_object_is_type(root, json_type_object) &&
            json_object_object_get_ex(root, norm, &value) && json_object_is_type(value, json_type_string))
        {
            const char* folder = json_object_get_string(value);
            if (folder != NULL && strlen(folder) <= MAP_NAME_MAX && map_name_ok(folder)) {
                snprintf(out, size, "%s", folder);
                ok = 1;
            }
        }
        if (root != NULL) {
            json_object_put(root);
        }
        free(text);
    }
    free(data);
    return ok;
}

int map_resolve_alias(const char* name, char* out, size_t size, int may_fetch)
{
    if (name == NULL || out == NULL || size == 0 || !map_name_ok(name) || map_installed(name)) {
        return 0;
    }
    char norm[MAP_NAME_MAX * 2];
    _norm(name, norm, sizeof norm);
    if (norm[0] == '\0') {
        return 0;
    }
    char        found[MAP_NAME_MAX + 1] = "";
    const char* by                      = NULL;
    if (map_user_dir_get()[0] != '\0' && _local_alias_in(map_user_dir_get(), norm, found, sizeof found)) {
        by = "local";
    }
    char data_maps[1100];
    data_path(data_maps, sizeof data_maps, "maps");
    if (by == NULL && _local_alias_in("Resources/maps", norm, found, sizeof found)) {
        by = "local";
    }
    if (by == NULL && strcmp(data_maps, "Resources/maps") != 0 &&
        _local_alias_in(data_maps, norm, found, sizeof found))
    {
        by = "local";
    }
    /* downloads off: only a folder that is already here (the repository is
       not asked for anything) */
    if (by == NULL && dl_enabled && _alias_from_file(norm, found, sizeof found, may_fetch)) {
        by = "repository";
    }
    if (by == NULL || strcmp(found, name) == 0) {
        return 0;
    }
    snprintf(out, size, "%s", found);
    LOG_INFO("Map '%s' is %s (alias)", name, found);
    evt_log(NULL, "map_download", "action=alias name=%s folder=%s by=%s", name, found, by);
    return 1;
}

static int _download(const char* name)
{
    if (!map_name_ok(name)) {
        LOG_WARNING("Map '%s' not downloaded: not a map name", name);
        evt_log(NULL, "map_download", "action=failed name=%s why=name", name);
        return 0;
    }
    char dir[600];
    if (!_target_dir(name, dir, sizeof dir)) {
        LOG_WARNING("Map '%s' not downloaded: neither Resources/maps nor the data folder can be written", name);
        evt_log(NULL, "map_download", "action=failed name=%s why=not_writable", name);
        return 0;
    }
    LOG_INFO("Downloading map %s from %smaps/%s/ ...", name, _base(), name);
    evt_log(NULL, "map_download", "action=start name=%s dir=%s", name, dir);
    uint64_t t0 = get_real_nanos();

    /* the repository keeps maps as ngmap at quality 100 (the geometry
       exact, a seventh of the bytes); a .vxl is still taken when there is
       no ngmap, from a fork of the repository or an older copy */
    const char* ext[]      = {"toml", "vxl", "txt"};
    long        max[]      = {MAP_TEXT_MAX, MAP_VXL_MAX, MAP_TEXT_MAX};
    int         got[3]     = {0, 0, 0};
    int         from_ngmap = 0;
    long        total      = 0;
    char        why[200]   = "";
    int         map_size[3];
    for (int k = 0; k < 3; k++) {
        char url[900], part[700];
        snprintf(part, sizeof part, "%s/%s.%s.part", dir, name, ext[k]);
        long bytes = 0;
        int  r     = 0;
        if (k == 1) {
            char ngpart[700];
            snprintf(url, sizeof url, "%smaps/%s/%s.ngmap", _base(), name, name);
            snprintf(ngpart, sizeof ngpart, "%s/%s.ngmap.part", dir, name);
            r = _fetch(url, ngpart, MAP_VXL_MAX, &bytes, why, sizeof why);
            if (r > 0) {
                from_ngmap = 1;
                if (!_ngmap_to_vxl(ngpart, part, map_size, why, sizeof why)) {
                    remove(ngpart);
                    break;
                }
                /* kept beside the VXL: the server loads the map from it and
                   sends it to 1.0 clients as it is */
            } else if (r < 0) {
                break;
            }
        }
        if (r == 0) {
            snprintf(url, sizeof url, "%smaps/%s/%s.%s", _base(), name, name, ext[k]);
            r = _fetch(url, part, max[k], &bytes, why, sizeof why);
        }
        if (r < 0 || (r == 0 && k < 2)) {
            if (r == 0) {
                snprintf(why, sizeof why, "not in the maps repository (%s.%s: 404)", name,
                         k == 1 ? "ngmap" : ext[k]);
            }
            break;
        }
        got[k] = r;
        total += bytes;
        if (k == 0 && !_check_toml(part, map_size, why, sizeof why)) {
            got[0] = 0;
            break;
        }
        if (k == 1 && !_check_vxl(part, map_size, why, sizeof why)) {
            got[1] = 0;
            break;
        }
    }
    if (!got[0] || !got[1]) {
        _remove3(dir, name);
        LOG_WARNING("Map %s could not be downloaded: %s", name, why);
        evt_log(NULL, "map_download", "action=failed name=%s why=\"%s\"", name, why);
        return 0;
    }
    /* the .vxl last: map_file_path takes a folder by its .vxl */
    const int order[] = {0, 2, 1};
    for (int i = 0; i < 3; i++) {
        int k = order[i];
        if (!got[k]) {
            continue;
        }
        char part[700], final[700];
        if (k == 1 && from_ngmap) {
            snprintf(part, sizeof part, "%s/%s.ngmap.part", dir, name);
            snprintf(final, sizeof final, "%s/%s.ngmap", dir, name);
            if (_move(part, final) != 0) {
                remove(part); /* the VXL is enough: its encoding is made at load */
            }
        }
        snprintf(part, sizeof part, "%s/%s.%s.part", dir, name, ext[k]);
        snprintf(final, sizeof final, "%s/%s.%s", dir, name, ext[k]);
        if (_move(part, final) != 0) {
            _remove3(dir, name);
            LOG_WARNING("Map %s could not be saved: %s: %s", name, final, strerror(errno));
            evt_log(NULL, "map_download", "action=failed name=%s why=rename", name);
            return 0;
        }
    }
    double secs = (double) (get_real_nanos() - t0) / 1e9;
    LOG_INFO("Map %s downloaded: %ld KB in %.1f s, in %s/", name, total / 1024, secs, dir);
    evt_log(NULL, "map_download", "action=done name=%s bytes=%ld ms=%ld txt=%d ngmap=%d dir=%s", name, total,
            (long) (secs * 1000), got[2], from_ngmap, dir);
    return 1;
}

/* a map folder with an ngmap and no VXL gets its VXL beside it, or in the
   download folder with its toml and txt when that folder is read only. No
   network: this is a map the host already has */
static int _unpack_local(const char* name)
{
    char src[1200], dir[600], from[1400], to[1400], why[200] = "";
    int  size[3];
    if (!map_name_ok(name) || !map_ngmap_folder(src, sizeof src, name)) {
        return 0;
    }
    snprintf(from, sizeof from, "%s/%s.toml", src, name);
    if (!_check_toml(from, size, why, sizeof why)) {
        LOG_WARNING("Map %s: %s", name, why);
        return 0;
    }
    snprintf(from, sizeof from, "%s/%s.ngmap", src, name);
    int beside = 0;
    {
        char probe[1400];
        snprintf(probe, sizeof probe, "%s/.write-test.%ld", src, _pid());
        FILE* fp = fopen(probe, "wb");
        if (fp != NULL) {
            fclose(fp);
            remove(probe);
            beside = 1;
        }
    }
    if (beside) {
        snprintf(dir, sizeof dir, "%s", src);
    } else if (!_target_dir(name, dir, sizeof dir)) {
        LOG_WARNING("Map %s: no folder to unpack its ngmap in", name);
        return 0;
    }
    if (!beside) {
        /* the copy's toml and txt go with its VXL: map_file_path takes a
           folder by its .vxl and reads the rest from it */
        const char* ext[] = {"toml", "txt"};
        for (int k = 0; k < 2; k++) {
            long     len  = 0;
            uint8_t* data = NULL;
            snprintf(to, sizeof to, "%s/%s.%s", src, name, ext[k]);
            if ((data = _read_all(to, MAP_TEXT_MAX, &len)) == NULL) {
                continue;
            }
            snprintf(to, sizeof to, "%s/%s.%s", dir, name, ext[k]);
            FILE* fp = fopen(to, "wb");
            if (fp != NULL) {
                fwrite(data, 1, (size_t) len, fp);
                fclose(fp);
            }
            free(data);
        }
    }
    uint64_t t0 = get_real_nanos();
    snprintf(to, sizeof to, "%s/%s.vxl.%ld.part", dir, name, _pid());
    if (!_ngmap_to_vxl(from, to, size, why, sizeof why) || !_check_vxl(to, size, why, sizeof why)) {
        remove(to);
        LOG_WARNING("Map %s: its ngmap could not be unpacked: %s", name, why);
        evt_log(NULL, "map_download", "action=unpack_failed name=%s why=\"%s\"", name, why);
        return 0;
    }
    char final[1400];
    snprintf(final, sizeof final, "%s/%s.vxl", dir, name);
    if (_move(to, final) != 0) {
        remove(to);
        return 0;
    }
    long ms = (long) ((get_real_nanos() - t0) / 1000000);
    LOG_INFO("Map %s unpacked from its ngmap in %ld ms, in %s/", name, ms, dir);
    evt_log(NULL, "map_download", "action=unpack name=%s ms=%ld dir=%s", name, ms, dir);
    return 1;
}

int map_download_now(const char* name)
{
    if (map_installed(name)) {
        return 1;
    }
    if (_unpack_local(name) && map_installed(name)) {
        return 1;
    }
    if (!dl_enabled) {
        return 0;
    }
    return _download(name) && map_installed(name);
}

static void* _background(void* arg)
{
    char* name = arg;
    /* off the tick, so a stale aliases.json may be fetched here: the admin's
       /map named a name the server did not know and gets the folder */
    char        folder[MAP_NAME_MAX + 1];
    const char* target = map_resolve_alias(name, folder, sizeof folder, 1) ? folder : name;
    if (!_unpack_local(target)) {
        _download(target);
    }
    free(name);
    pthread_mutex_lock(&dl_lock);
    dl_pending[0] = '\0';
    pthread_mutex_unlock(&dl_lock);
    return NULL;
}

int map_download_background(const char* name)
{
    if (!dl_enabled || !map_name_ok(name)) {
        return 0;
    }
    pthread_mutex_lock(&dl_lock);
    if (dl_pending[0] != '\0') {
        pthread_mutex_unlock(&dl_lock);
        return 0;
    }
    snprintf(dl_pending, sizeof dl_pending, "%s", name);
    pthread_mutex_unlock(&dl_lock);
    pthread_t t;
    char*     copy = strdup(name);
    if (copy == NULL || pthread_create(&t, NULL, _background, copy) != 0) {
        free(copy);
        pthread_mutex_lock(&dl_lock);
        dl_pending[0] = '\0';
        pthread_mutex_unlock(&dl_lock);
        return 0;
    }
    pthread_detach(t);
    return 1;
}

const char* map_download_pending(void)
{
    static char copy[MAP_NAME_MAX + 1];
    pthread_mutex_lock(&dl_lock);
    snprintf(copy, sizeof copy, "%s", dl_pending);
    pthread_mutex_unlock(&dl_lock);
    return copy;
}
