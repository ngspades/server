// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PTB_H
#define PTB_H

/*
 * Plant the Bomb (mode "ptb"): Counter-Strike's bomb defusal rounds on the
 * arena's maps. Team 1 are the counter-terrorists, who defend two bombsites
 * (A and B); team 2 the terrorists, one of whom carries the bomb. A round is
 * a freeze (the arena's closed gates), then round_seconds of fight:
 *
 *   the terrorists win by killing every counter-terrorist, or when the bomb
 *   they planted in a site explodes (bomb_seconds after the plant);
 *   the counter-terrorists win by killing every terrorist before the plant,
 *   by defusing the planted bomb, or when the clock runs out unplanted.
 *
 * Planting is holding USE for plant_seconds standing still inside a site;
 * defusing, holding USE for defuse_seconds within reach of the bomb. Moving
 * away or taking a hit starts it over. A dead carrier drops the bomb, which
 * any terrorist picks up by walking over it. rounds_to_win rounds win the
 * match. The sides never swap.
 *
 * The sites: the map's toml [ptb] sites, else Resources/ptb_sites.toml (the
 * catalog's table, kept out of the maps so a downloaded map keeps them),
 * else two places picked between the spawns (docs/ptb-sites.md).
 *
 * Wire: GameModeState mode=ptb with the arena's round keys (arena-state,
 * -round, -time, -winner) and the ptb-* keys; ptb-plant and ptb-defuse
 * GenericCommands to the one holding USE; the client sends "ptb-use 1|0"
 * (docs/protocol-1.0.md). The planted bomb is a P10_ENT_DYNAMITE that no
 * shot sets off. EVT ptb action=...
 */

#include <Server/Gamemodes/Gamemode.h>

extern const gamemode_ops_t ptb_ops;

#endif
