// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A small, independent implementation of the shared-bridge idea in
 * BattleSpades (AGPL-3.0). It is not a copy of that code, which cannot be
 * merged into this GPL-3.0 tree; it follows the behaviour described in its
 * docs with this server's own block action and planner.
 */

#include <Server/MapStore.h>
#include <Server/BotProjects.h>
#include <Server/Profile.h>
#include <Server/BotsInternal.h>
#include <Server/EventLog.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <math.h>
#include <string.h>

typedef struct
{
    uint8_t  used;
    uint8_t  team;
    uint8_t  owner;
    uint8_t  partner;
    uint16_t id;
    uint8_t  count;
    uint8_t  built[BOT_PROJECT_CELLS];
    uint8_t  complete;
    int      cells[BOT_PROJECT_CELLS][3];
    int      landing[3];
    uint64_t expires;
    uint64_t partner_deadline;
    uint64_t next_log;
} bot_project_t;

static bot_project_t projects[BOT_PROJECTS];
static uint16_t       next_id = 1;
static uint8_t        enabled = 1;

void bot_projects_init(server_t* server)
{
    (void) server;
    enabled = 1;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "bots") : NULL;
        toml_datum_t  d = t != NULL ? toml_bool_in(t, "cooperative_bridges") : (toml_datum_t) {0};
        if (d.ok) {
            enabled = d.u.b ? 1 : 0;
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_BOT_PROJECTS");
    if (env != NULL && *env != '\0') {
        enabled = atoi(env) != 0;
    }
}

static bot_project_t* _by_bot(uint8_t id)
{
    for (int k = 0; k < BOT_PROJECTS; k++) {
        if (projects[k].used &&
            (projects[k].owner == id || (projects[k].partner != 0xFF && projects[k].partner == id))) {
            return &projects[k];
        }
    }
    return NULL;
}

static int _near_project(uint8_t team, int x, int y)
{
    for (int k = 0; k < BOT_PROJECTS; k++) {
        if (!projects[k].used || projects[k].team != team) {
            continue;
        }
        int dx = projects[k].cells[0][0] - x, dy = projects[k].cells[0][1] - y;
        if (dx * dx + dy * dy < 20 * 20) {
            return 1;
        }
    }
    return 0;
}

void bot_projects_reset(void)
{
    memset(projects, 0, sizeof projects);
    next_id = 1;
}

uint8_t bot_project_create_bridge(server_t* server, player_t* bot, int x, int y, int z, int dx, int dy)
{
    if (!enabled || bot == NULL || !bot->is_bot || (dx == 0 && dy == 0) || _by_bot(bot->id) != NULL ||
        _near_project(bot->team, x, y))
    {
        return 0;
    }
    if (z < 1 || z > (int) server->s_map.map.size_z - 3) {
        return 0;
    }
    int slot = -1;
    for (int k = 0; k < BOT_PROJECTS; k++) {
        if (!projects[k].used) {
            slot = k;
            break;
        }
    }
    if (slot < 0) {
        return 0;
    }
    bot_project_t* p = &projects[slot];
    memset(p, 0, sizeof *p);
    p->used  = 1;
    p->team    = bot->team;
    p->owner   = bot->id;
    p->partner = 0xFF;
    p->id      = next_id++;
    for (int k = 0; k < BOT_PROJECT_CELLS; k++) {
        int cx = x + dx * k, cy = y + dy * k;
        if (cx < 2 || cy < 2 || cx >= (int) server->s_map.map.size_x - 2 ||
            cy >= (int) server->s_map.map.size_y - 2)
            break;
        if (map_solid(&server->s_map.map, (uint16_t) cx, (uint16_t) cy, (uint16_t) z)) {
            /* the far bank: the landing is the solid column, the cells before
               it are the bridge */
            p->landing[0] = cx;
            p->landing[1] = cy;
            p->landing[2] = z;
            break;
        }
        p->cells[p->count][0] = cx;
        p->cells[p->count][1] = cy;
        p->cells[p->count][2] = z;
        p->count++;
    }
    if (p->count == 0) {
        memset(p, 0, sizeof *p);
        return 0;
    }
    if (p->landing[2] == 0) {
        p->landing[0] = x + dx * (p->count + 1);
        p->landing[1] = y + dy * (p->count + 1);
        p->landing[2] = z;
    }
    /* one partner: the nearest living bot of the same team that is not on a
       project yet. It gets the far cells; the owner keeps the near ones. */
    float best = 16.0f * 16.0f;
    player_t *other, *tmp;
    HASH_ITER(hh, server->players, other, tmp)
    {
        if (other == bot || !other->is_bot || other->team != bot->team || other->state != STATE_READY || !other->alive ||
            other->blocks == 0 || _by_bot(other->id) != NULL)
        {
            continue;
        }
        float ox = other->movement.position.x - bot->movement.position.x;
        float oy = other->movement.position.y - bot->movement.position.y;
        float d  = ox * ox + oy * oy;
        if (d < best) {
            best         = d;
            p->partner   = other->id;
        }
    }
    p->expires          = get_nanos() + 30ULL * NANO_IN_SECOND;
    p->partner_deadline = get_nanos() + 8ULL * NANO_IN_SECOND;
    evt_log(bot, "bot_project", "action=create kind=bridge id=%u partner=%u cells=%u landing=%d,%d,%d", p->id,
            p->partner, p->count, p->landing[0], p->landing[1], p->landing[2]);
    return 1;
}

static int _assigned(const bot_project_t* p, uint8_t id, int* index)
{
    int partner = (p->partner != 0xFF && id == p->partner);
    for (int k = 0; k < p->count; k++) {
        if (p->built[k]) {
            continue;
        }
        /* the line is built from the near bank out: the first unbuilt cell's
           owner is the only one who may place it. Without a partner the owner
           builds every cell; with one they alternate. */
        if (p->partner == 0xFF || (k & 1) == partner) {
            *index = k;
            return 1;
        }
        return 0;
    }
    return 0;
}

uint8_t bot_project_step(server_t* server, player_t* bot, int goal_floor)
{
    (void) goal_floor;
    bot_project_t* p = _by_bot(bot->id);
    if (p == NULL || p->complete) {
        return 0;
    }
    int index;
    if (!_assigned(p, bot->id, &index)) {
        return 0;
    }
    int* c = p->cells[index];
    if (map_solid(&server->s_map.map, (uint16_t) c[0], (uint16_t) c[1], (uint16_t) c[2])) {
        p->built[index] = 1;
        return 0;
    }
    float dx = (float) c[0] + 0.5f - bot->movement.position.x;
    float dy = (float) c[1] + 0.5f - bot->movement.position.y;
    if (dx * dx + dy * dy > 4.5f * 4.5f || bot->blocks == 0) {
        return 0;
    }
    _bot_set_tool(server, bot, TOOL_BLOCK);
    _bot_face(bot, c[0] + 0.5f, c[1] + 0.5f, (float) c[2]);
    uint8_t ok = _bot_place_block(server, bot, (uint32_t) c[0], (uint32_t) c[1], (uint32_t) c[2]);
    if (ok) {
        p->built[index] = 1;
        p->next_log     = 0;
        evt_log(bot, "bot_project", "action=cell id=%u cell=%d,%d,%d ok=1 index=%d", p->id, c[0], c[1], c[2], index);
        return 1;
    }
    uint64_t now = get_nanos();
    if (now >= p->next_log) {
        p->next_log = now + NANO_IN_SECOND;
        evt_log(bot, "bot_project", "action=cell id=%u cell=%d,%d,%d ok=0 index=%d", p->id, c[0], c[1], c[2], index);
    }
    return 0;
}

uint8_t bot_project_goal(server_t* server, player_t* bot, float* x, float* y, int* floor)
{
    (void) server;
    bot_project_t* p = _by_bot(bot->id);
    if (p == NULL) {
        return 0;
    }
    if (p->complete) {
        if (p->partner != 0xFF && bot->id == p->partner) {
            float dx = (float) p->landing[0] + 0.5f - bot->movement.position.x;
            float dy = (float) p->landing[1] + 0.5f - bot->movement.position.y;
            if (dx * dx + dy * dy > 2.0f * 2.0f) {
                *x     = p->landing[0] + 0.5f;
                *y     = p->landing[1] + 0.5f;
                *floor = p->landing[2];
                return 1;
            }
        }
        return 0;
    }
    /* both build from the near bank out: the first unbuilt cell is where
       both wait, and step() lets only its owner place it */
    for (int k = 0; k < p->count; k++) {
        if (!p->built[k]) {
            *x     = p->cells[k][0] + 0.5f;
            *y     = p->cells[k][1] + 0.5f;
            *floor = p->cells[k][2];
            return 1;
        }
    }
    return 0;
}

void bot_projects_tick(server_t* server)
{
    uint64_t now = get_nanos();
    for (int k = 0; k < BOT_PROJECTS; k++) {
        bot_project_t* p = &projects[k];
        if (!p->used) {
            continue;
        }
        int done = 1;
        for (int i = 0; i < p->count; i++) {
            if (!p->built[i] &&
                map_solid(&server->s_map.map, (uint16_t) p->cells[i][0], (uint16_t) p->cells[i][1],
                                (uint16_t) p->cells[i][2]))
            {
                p->built[i] = 1;
            }
            done &= p->built[i];
        }
        /* a partner that died or stayed away must not hold the crossing:
           after eight seconds the owner builds every remaining cell, and a
           dead owner hands the whole project to a living partner */
        player_t* owner = NULL;
        player_t* partner = NULL;
        HASH_FIND(hh, server->players, &p->owner, sizeof(p->owner), owner);
        if (p->partner != 0xFF) {
            HASH_FIND(hh, server->players, &p->partner, sizeof(p->partner), partner);
        }
        int owner_ok   = owner != NULL && owner->state == STATE_READY && owner->alive;
        int partner_ok = partner != NULL && partner->state == STATE_READY && partner->alive;
        if (!owner_ok && partner_ok) {
            p->owner   = p->partner;
            p->partner = 0xFF;
            evt_log(partner, "bot_project", "action=takeover id=%u", p->id);
        } else if (p->partner != 0xFF && (!partner_ok || now >= p->partner_deadline)) {
            evt_log(owner, "bot_project", "action=partner_timeout id=%u partner=%u", p->id, p->partner);
            p->partner = 0xFF;
        }
        if (done && !p->complete) {
            p->complete = 1;
            p->expires  = now + 15ULL * NANO_IN_SECOND;
            player_t* owner = NULL;
            HASH_FIND(hh, server->players, &p->owner, sizeof(p->owner), owner);
            evt_log(owner, "bot_project", "action=built id=%u cells=%u landing=%d,%d,%d", p->id, p->count,
                    p->landing[0], p->landing[1], p->landing[2]);
        }
        if (now >= p->expires) {
            player_t* owner = NULL;
            HASH_FIND(hh, server->players, &p->owner, sizeof(p->owner), owner);
            evt_log(owner, "bot_project", "action=expire id=%u complete=%u built=%u/%u", p->id, p->complete,
                    p->built[0] + p->built[1] + p->built[2] + p->built[3] + p->built[4] + p->built[5], p->count);
            memset(p, 0, sizeof *p);
        }
    }
}
