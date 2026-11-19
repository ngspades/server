// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/BlockHealth.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>

#include <classes/classes.h>
#include <Server/MapStore.h>
#include <string.h>

/* A small table, not one byte per cell of the map: only the blocks hit in
   the last GAME_BLOCK_HEAL_SECONDS are in it, a few dozen in a busy match.
   Open addressing over BH_PROBE slots from the cell's hash, every one of
   them looked at (no early stop), so a slot is freed by clearing it, and a
   full run evicts its oldest crack: a hit never fails, at worst a crack
   elsewhere heals early. */
#define BH_SLOTS 4096
#define BH_PROBE 16
#define BH_HEAL_NS ((uint64_t) GAME_BLOCK_HEAL_SECONDS * NANO_IN_SECOND)

typedef struct
{
    uint64_t key; /* 0: free */
    uint64_t at;  /* get_nanos() of the last hit */
    uint32_t color;
    uint8_t  damage;
} block_health_t;

static block_health_t bh_table[BH_SLOTS];
static uint32_t       bh_serial;

static uint64_t _key(int x, int y, int z)
{
    return (((uint64_t) (uint32_t) x << 32) | ((uint64_t) (uint16_t) y << 8) | (uint64_t) (uint8_t) z) + 1;
}

static uint32_t _home(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (uint32_t) key & (BH_SLOTS - 1);
}

static block_health_t* _find(uint64_t key)
{
    uint32_t h = _home(key);
    for (int i = 0; i < BH_PROBE; i++) {
        block_health_t* e = &bh_table[(h + i) & (BH_SLOTS - 1)];
        if (e->key == key) {
            return e;
        }
    }
    return NULL;
}

/* the damage the cell has taken now: 0 for a crack healed or on another
   block (a block of another colour: a path that changed the cell without
   proto10_world_cell would otherwise hand it the crack) */
static int _bh_taken(server_t* server, const block_health_t* e, int x, int y, int z, uint64_t now)
{
    if (e == NULL || now - e->at > BH_HEAL_NS || e->color != map_color(&server->s_map.map, x, y, z)) {
        return 0;
    }
    return e->damage;
}

static int _add(int taken, int damage)
{
    taken += damage > 0 ? damage : 0;
    return taken > GAME_BLOCK_BREAK ? GAME_BLOCK_BREAK : taken;
}

int block_health_peek(server_t* server, int x, int y, int z, int damage)
{
    return _add(_bh_taken(server, _find(_key(x, y, z)), x, y, z, get_nanos()), damage);
}

int block_health_taken(server_t* server, int x, int y, int z)
{
    return _bh_taken(server, _find(_key(x, y, z)), x, y, z, get_nanos());
}

void block_health_reset(void)
{
    memset(bh_table, 0, sizeof bh_table);
}

int block_health_hit(server_t* server, int x, int y, int z, int damage)
{
    uint64_t        now   = get_nanos();
    uint64_t        key   = _key(x, y, z);
    block_health_t* e     = _find(key);
    int             taken = _add(_bh_taken(server, e, x, y, z, now), damage);
    if (e == NULL) {
        uint32_t h = _home(key);
        for (int i = 0; i < BH_PROBE; i++) {
            block_health_t* s = &bh_table[(h + i) & (BH_SLOTS - 1)];
            if (s->key == 0 || now - s->at > BH_HEAL_NS) {
                e = s;
                break;
            }
            if (e == NULL || s->at < e->at) {
                e = s; /* the oldest, should the whole run be fresh */
            }
        }
        e->key = key;
    }
    e->damage = (uint8_t) taken;
    e->at     = now;
    e->color  = map_color(&server->s_map.map, x, y, z);
    if (taken >= GAME_BLOCK_BREAK) {
        e->key = 0; /* it breaks: the next block there starts whole */
    } else {
        bh_serial++;
    }
    return taken;
}

void block_health_forget(server_t* server, int x, int y, int z)
{
    (void) server;
    block_health_t* e = _find(_key(x, y, z));
    if (e != NULL) {
        e->key = 0;
    }
}

uint32_t block_health_serial(void)
{
    return bh_serial;
}
