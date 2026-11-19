// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The sentry gun's size and reach, shared by the server (it places it, hits
 * it, and its shots come from that box: server/Source/Server/Proto10/
 * Sentry.h) and the client (it draws it, marks it and previews the cell the
 * player looks at: the client's sentry.c, sentry_cell_valid).
 *
 * The client's placement preview must refuse exactly the cells the server
 * refuses, and the mark and the health bar must sit on the box the server
 * shoots at: the two headers held the same numbers twice, so a change to
 * one left the preview promising a sentry the server would refuse, or a
 * mark floating beside it.
 *
 * Header only: std C99, no dependency on either program.
 */
#ifndef SHARED_SENTRY_H
#define SHARED_SENTRY_H

#define GAME_SENTRY_HEALTH      200
#define GAME_SENTRY_HALF_WIDTH  0.45f
#define GAME_SENTRY_HEIGHT      1.25f
/* eye to the top centre of the floor block, the farthest a sentry is placed */
#define GAME_SENTRY_PLACE_RANGE 5.0f

#endif
