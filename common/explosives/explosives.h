// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The Marksman's landmine and the Miner's dynamite: the boxes a shot meets and
 * the dynamite's fuse. Shared by the server
 * (server/Source/Server/Proto10/Explosives.c, which places the object with
 * these sizes and blows it up after the fuse) and the client
 * (the client's explosives.c, which draws it and burns the fuse down; how close
 * an enemy's mine is drawn is the client's own, the client's explosives.h).
 *
 * The server spelled the sizes as literals in its placement call and the
 * client had its own copy in a comment ("the server's sizes"): a change to
 * one drew a mine beside the box a shot meets.
 *
 * Header only: std C99, no dependency on either program.
 */
#ifndef SHARED_EXPLOSIVES_H
#define SHARED_EXPLOSIVES_H

#define GAME_LANDMINE_HALF_WIDTH 0.4f
#define GAME_LANDMINE_HEIGHT     0.2f
#define GAME_DYNAMITE_HALF_WIDTH 0.4f
#define GAME_DYNAMITE_HEIGHT     0.6f

/* seconds from its placement to the blast */
#define GAME_DYNAMITE_FUSE 4.0f

#endif
