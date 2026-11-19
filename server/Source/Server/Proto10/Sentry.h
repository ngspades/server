// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_SENTRY_H
#define PROTO10_SENTRY_H

/*
 * The sentry gun (docs/protocol-1.0.md 12.5), a perk (Perks.c). A world
 * object (Objects.c) of type P10_ENT_SENTRY standing on a solid block: it
 * turns towards the nearest visible enemy, fires with an honest aim error
 * and hurts through send_set_hp with its owner as the attacker, so kills,
 * scores and the kill feed credit the owner (kill reason 7).
 *
 * The placement rules are mirrored by the client's preview
 * (the client's sentry.c, sentry_cell_valid): change both together.
 */

#include <Server/Proto10/Objects.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <sentry/sentry.h>

/* [1.0] kill reason after the 0.75 ones (0..6): killed by a sentry gun */
#define KILL_REASON_SENTRY 7

/* the box, the health and the reach are common/sentry/sentry.h, shared with
   the client's drawing and its placement preview: read GAME_SENTRY_* there */
/* how long an untouched sentry lasts, when [sentry] lifetime_seconds is unset */
#define SENTRY_LIFETIME_DEFAULT 60.0f

/* reads the optional [sentry] table of Resources/config.toml */
void sentry_init(server_t* server);

/* the sentry rules as GameStateFinal world parameters ("sentry-lifetime"), so
   the client can show the remaining seconds on its label */
void sentry_put_world_params(server_t* server, p10_writer* w);

/* the floor block (x, y, z) a sentry would stand on; 1 when it may, else
   *why says why not (a short token used in EVT lines and on the HUD) */
int sentry_cell_ok(server_t* server, player_t* player, int x, int y, int z, const char** why);
/* places the owner's sentry; NULL and *why when refused */
p10_object_t* sentry_place(server_t* server, player_t* owner, int x, int y, int z, const char** why);

#endif
