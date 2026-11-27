// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_GAMEMODES_TC_H
#define SERVER_GAMEMODES_TC_H

#include <Server/Gamemodes/Gamemode.h>

extern const gamemode_ops_t tc_ops;

/* the territories as the 0.75 wire needs them: how many, and the cell and
   holder (0, 1, or 2 for nobody) of one */
int tc_count(void);
int tc_territory(int i, float* x, float* y, float* z);

#endif
