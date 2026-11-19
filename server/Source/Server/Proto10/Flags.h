// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_FLAGS_H
#define PROTO10_FLAGS_H

/*
 * The objectives as entities (ngspades' design, docs/protocol-1.0.md 12.2.3):
 * each intel is a Flag and each tent a CommandPost, with a team, a Constant
 * trajectory at its cell and, for a carried intel, its carrier as the
 * trajectory's parentEntityId. GameModeState keeps only what an entity
 * cannot say (the intel's state word, the scores): one source of truth.
 *
 * A mode says where its objectives are while GameModeState is computed
 * (proto10_flags_begin, then proto10_flags_set from its state_keys hook);
 * proto10_flags_send then sends what changed, before the GameModeState that
 * goes with it, so a client reading "carried" already has the carrier.
 */

#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <stdint.h>

/* the intels (Flag), the tents (CommandPost: a team's fixed base) and what
   is taken by standing on it (Checkpoint: KotH's hill, territory control's
   territories; neutral). A slot's entity id is P10_OBJECTIVE_ID_BASE + slot */
enum {
    P10_FLAG_INTEL1,
    P10_FLAG_INTEL2,
    P10_FLAG_TENT1,
    P10_FLAG_TENT2,
    P10_FLAG_HILL,
    P10_FLAG_TERRITORY0 = P10_OBJECTIVE_TERRITORY0,
    P10_FLAG_SLOTS      = P10_OBJECTIVE_TERRITORY0 + P10_TERRITORIES_MAX
};

/* every slot not set again before the next send is removed */
void proto10_flags_begin(void);
/* slot's position (map coordinates, z down: the cell it rests on), carried
   by player carrier (-1: on the ground) */
void proto10_flags_set(int slot, float x, float y, float z, int carrier);
/* EntityUpdate (and EntityRemove) of what changed, to every 1.0 client */
void proto10_flags_send(server_t* server);
/* GameStateFinal */
uint32_t proto10_flags_count(void);
void     proto10_flags_put(p10_writer* w);

#endif
