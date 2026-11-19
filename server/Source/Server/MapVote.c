// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Map.h>
#include <Server/MapDownload.h>
#include <Server/MapVote.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/DataDir.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <mersenne-twister/MT.h>
#include <uthash/uthash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

#define STATS_MAX 512

/* matches played a map, by name: the share the vote shows */
static struct
{
    char     name[MAP_NAME_MAX + 1];
    uint32_t matches;
} stats[STATS_MAX];
static int stats_count;
static int stats_loaded;

static struct
{
    int      active; /* options offered, votes taken */
    int      closed; /* the result went out */
    int      count;
    char     name[MAP_VOTE_OPTIONS][MAP_NAME_MAX + 1];
    int      size[MAP_VOTE_OPTIONS];
    int      percent[MAP_VOTE_OPTIONS];
    int8_t   vote_of[256]; /* by player id: an option, -1 none */
    uint64_t closes_at;
} mv;

static const char* _stats_file(char* out, size_t size)
{
    return data_path(out, size, "map-stats.txt");
}

static void _stats_load(void)
{
    if (stats_loaded) {
        return;
    }
    stats_loaded = 1;
    stats_count  = 0;
    char path[512];
    FILE* fp = fopen(_stats_file(path, sizeof path), "r");
    if (fp == NULL) {
        return;
    }
    char     line[256];
    char     name[MAP_NAME_MAX + 1];
    unsigned n;
    while (stats_count < STATS_MAX && fgets(line, sizeof line, fp) != NULL) {
        if (sscanf(line, "%64s %u", name, &n) == 2 && n > 0) {
            snprintf(stats[stats_count].name, sizeof stats[stats_count].name, "%s", name);
            stats[stats_count].matches = n;
            stats_count++;
        }
    }
    fclose(fp);
}

static void _stats_save(void)
{
    char path[512], tmp[560];
    _stats_file(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE* fp = fopen(tmp, "w");
    if (fp == NULL) {
        return;
    }
    for (int k = 0; k < stats_count; k++) {
        fprintf(fp, "%s %u\n", stats[k].name, (unsigned) stats[k].matches);
    }
    fclose(fp);
    rename(tmp, path);
}

static uint32_t _stats_total(void)
{
    uint32_t total = 0;
    for (int k = 0; k < stats_count; k++) {
        total += stats[k].matches;
    }
    return total;
}

static uint32_t _stats_of(const char* name)
{
    for (int k = 0; k < stats_count; k++) {
        if (!strcmp(stats[k].name, name)) {
            return stats[k].matches;
        }
    }
    return 0;
}

void mapvote_record_match(server_t* server)
{
    _stats_load();
    const char* name = server->map_name;
    if (name[0] == '\0') {
        return;
    }
    for (int k = 0; k < stats_count; k++) {
        if (!strcmp(stats[k].name, name)) {
            stats[k].matches++;
            _stats_save();
            return;
        }
    }
    if (stats_count < STATS_MAX) {
        snprintf(stats[stats_count].name, sizeof stats[stats_count].name, "%s", name);
        stats[stats_count].matches = 1;
        stats_count++;
        _stats_save();
    }
}

/* the map's horizontal size from its toml: 512 unless it says another */
static int _map_size_of(const char* name)
{
    char path[512];
    map_file_path(path, sizeof path, name, "toml");
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return 512;
    }
    char          err[200];
    toml_table_t* root = toml_parse_file(fp, err, sizeof err);
    fclose(fp);
    int size = 512;
    if (root != NULL) {
        toml_table_t* map = toml_table_in(root, "map");
        toml_array_t* a   = map != NULL ? toml_array_in(map, "map_size") : NULL;
        if (a != NULL) {
            toml_datum_t d = toml_int_at(a, 0);
            if (d.ok && d.u.i > 0 && d.u.i <= 4096) {
                size = (int) d.u.i;
            }
        }
        toml_free(root);
    }
    return size;
}

int mapvote_open(void)
{
    return mv.active && !mv.closed;
}

void mapvote_reset(void)
{
    memset(&mv, 0, sizeof mv);
}

static int _tally(server_t* server, int* counts)
{
    int       voters = 0;
    player_t *p, *tmp;
    for (int k = 0; k < MAP_VOTE_OPTIONS; k++) {
        counts[k] = 0;
    }
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot || p->conn == NULL || p->state == STATE_DISCONNECTED) {
            continue;
        }
        int v = mv.vote_of[p->id];
        if (v >= 0 && v < mv.count) {
            counts[v]++;
            voters++;
        }
    }
    return voters;
}

int mapvote_votes(server_t* server, int index)
{
    int counts[MAP_VOTE_OPTIONS];
    if (!mv.active || index < 0 || index >= mv.count) {
        return -1;
    }
    _tally(server, counts);
    return counts[index];
}

static void _send_tally(server_t* server, player_t* to)
{
    int  counts[MAP_VOTE_OPTIONS];
    char text[MAP_VOTE_OPTIONS][8];
    const char* parts[1 + MAP_VOTE_OPTIONS];
    _tally(server, counts);
    parts[0] = "vote-tally";
    for (int k = 0; k < mv.count; k++) {
        snprintf(text[k], sizeof text[k], "%d", counts[k]);
        parts[1 + k] = text[k];
    }
    proto10_bridge_generic(server, to, 1 + mv.count, parts);
}

static void _send_options(server_t* server, player_t* to, int seconds_left)
{
    char secs[12], count[12];
    snprintf(secs, sizeof secs, "%d", seconds_left);
    snprintf(count, sizeof count, "%d", mv.count);
    const char* start[] = {"vote-start", secs, count};
    proto10_bridge_generic(server, to, 3, start);
    for (int k = 0; k < mv.count; k++) {
        char idx[12], size[12], percent[12];
        snprintf(idx, sizeof idx, "%d", k);
        snprintf(size, sizeof size, "%d", mv.size[k]);
        snprintf(percent, sizeof percent, "%d", mv.percent[k]);
        const char* parts[] = {"vote-map", idx, mv.name[k], size, percent};
        proto10_bridge_generic(server, to, 5, parts);
    }
    _send_tally(server, to);
}

/* the rotation's maps but the one just played, each once */
static int _candidates(server_t* server, const char** cand, int max)
{
    int            n = 0;
    string_node_t* node;
    DL_FOREACH(server->s_map.map_list, node)
    {
        if (n < max && strcmp(node->string, server->map_name) != 0) {
            int dup = 0;
            for (int k = 0; k < n; k++) {
                dup = dup || !strcmp(cand[k], node->string);
            }
            if (!dup) {
                cand[n++] = node->string;
            }
        }
    }
    return n;
}

int mapvote_possible(server_t* server)
{
    const char* cand[256];
    return !profile_is_075() && _candidates(server, cand, 256) >= 2;
}

int mapvote_start(server_t* server, int summary_seconds)
{
    mapvote_reset();
    if (profile_is_075()) {
        return 0;
    }
    _stats_load();
    /* in a random order (a partial Fisher-Yates over the candidates) */
    const char* cand[256];
    int         n = _candidates(server, cand, 256);
    if (n < 2) {
        return 0; /* nothing to choose between */
    }
    int take = n < MAP_VOTE_OPTIONS ? n : MAP_VOTE_OPTIONS;
    for (int k = 0; k < take; k++) {
        int j = k + (int) (gen_rand_long(&server->rand) % (unsigned long) (n - k));
        const char* t = cand[k];
        cand[k]       = cand[j];
        cand[j]       = t;
    }
    uint32_t total = _stats_total();
    for (int k = 0; k < take; k++) {
        snprintf(mv.name[k], sizeof mv.name[k], "%s", cand[k]);
        mv.size[k]    = _map_size_of(cand[k]);
        mv.percent[k] = total > 0 ? (int) (((uint64_t) _stats_of(cand[k]) * 100 + total / 2) / total) : 0;
    }
    mv.count = take;
    memset(mv.vote_of, -1, sizeof mv.vote_of);
    mv.active    = 1;
    mv.closes_at = get_nanos() + (uint64_t) (summary_seconds - MAP_VOTE_RESULT_SECONDS) * NANO_IN_SECOND;
    _send_options(server, NULL, summary_seconds - MAP_VOTE_RESULT_SECONDS);
    evt_log(NULL, "mapvote", "action=start options=%d seconds=%d played=%u", take,
            summary_seconds - MAP_VOTE_RESULT_SECONDS, (unsigned) total);
    return 1;
}

int mapvote_cast(server_t* server, player_t* player, int index)
{
    if (!mapvote_open() || player == NULL || player->is_bot || index < 0 || index >= mv.count) {
        evt_log(player, "mapvote", "action=refused why=%s index=%d", mapvote_open() ? "bad_index" : "closed", index);
        return 0;
    }
    if (mv.vote_of[player->id] == index) {
        return 1;
    }
    mv.vote_of[player->id] = (int8_t) index;
    evt_log(player, "mapvote", "action=vote index=%d map=%s", index, mv.name[index]);
    _send_tally(server, NULL);
    return 1;
}

void mapvote_tick(server_t* server)
{
    if (!mapvote_open() || get_nanos() < mv.closes_at) {
        return;
    }
    int counts[MAP_VOTE_OPTIONS];
    int voters = _tally(server, counts);
    int best = 0, tied[MAP_VOTE_OPTIONS], ties = 0;
    for (int k = 0; k < mv.count; k++) {
        best = counts[k] > best ? counts[k] : best;
    }
    for (int k = 0; k < mv.count; k++) {
        if (counts[k] == best) {
            tied[ties++] = k; /* with no vote every option is "tied" at 0 */
        }
    }
    int win = tied[ties > 1 ? (int) (gen_rand_long(&server->rand) % (unsigned long) ties) : 0];
    mv.closed = 1;
    if (!server_force_map(server, mv.name[win])) {
        LOG_WARNING("Map vote: %s cannot be loaded, the rotation goes on", mv.name[win]);
        evt_log(NULL, "mapvote", "action=failed map=%s", mv.name[win]);
        win = -1;
    }
    char idx[12];
    snprintf(idx, sizeof idx, "%d", win);
    const char* parts[] = {"vote-result", idx};
    proto10_bridge_generic(server, NULL, 2, parts);
    evt_log(NULL, "mapvote", "action=result index=%d map=%s votes=%d voters=%d tied=%d", win,
            win >= 0 ? mv.name[win] : "-", win >= 0 ? counts[win] : 0, voters, ties);
    if (win >= 0) {
        broadcast_server_notice(server, 0, "Next map: %s", mv.name[win]);
    }
}
