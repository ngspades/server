// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The machine gun emplacement, the Commando's streak perk (it replaced the
 * revive): its size, reach and fire, shared by the server (it places it,
 * aims it with its gunner's look, fires it and is hit through this box:
 * server/Source/Server/Proto10/Mg.c) and the client (it draws it, previews
 * the cell and plays its shots: the client's mg.c).
 *
 * The figures are the AoS 1.0 beta client's (aosogl, aoslib/scenes/main.py
 * MachineGun and constants.py: MG_SHOOT_RATE 0.2, the SMG's damage table and
 * tracer, models drawn at 0.06 a voxel), credited in README.md and CREDITS.md.
 *
 * Header only: std C99, no dependency on either program.
 */
#ifndef SHARED_MG_H
#define SHARED_MG_H

#define GAME_MG_HEALTH        250
#define GAME_MG_HALF_WIDTH    0.5f
#define GAME_MG_HEIGHT        1.4f
/* eye to the top centre of the floor block, the farthest an MG is placed */
#define GAME_MG_PLACE_RANGE   5.0f
/* a gunner mounts from this far (eye to the MG's middle) */
#define GAME_MG_USE_RANGE     2.5f
/* 10 rounds a second, the SMG's rate: 5 a second read as sluggish next to
   the win32 client's emplacement, which is the gun this one plays as */
#define GAME_MG_FIRE_INTERVAL 0.1f
/* per round, the SMG's table: body, head, arms, legs */
#define GAME_MG_DAMAGE_BODY   24
#define GAME_MG_DAMAGE_HEAD   75
#define GAME_MG_DAMAGE_LIMB   16
/* the arc the barrel turns, either side of the way it was placed facing,
   and how far it tilts up and down (degrees) */
#define GAME_MG_YAW_ARC       90.0f
#define GAME_MG_PITCH_UP      30.0f
#define GAME_MG_PITCH_DOWN    25.0f
/* the barrel's recoil per round (aosogl: recoil_up 2, recoil_side 1.5, degrees) */
#define GAME_MG_RECOIL_UP     2.0f
#define GAME_MG_RECOIL_SIDE   1.5f
/* model scale, a voxel in blocks (aosogl's MachineGun.size) */
#define GAME_MG_VOXEL         0.06f
/* the muzzle, from the swivel along the barrel (blocks) */
#define GAME_MG_MUZZLE        1.8f

/* The barrel heats as it fires (Battlefield's emplacements): every round adds
   HEAT_PER_SHOT of a full barrel, 1.0 is overheated, and the gun then stops
   until it has cooled to HEAT_RESUME. Cooling is slow while it could fire
   again and quicker once it has jammed, so holding the trigger to the end is
   punished with a pause and a short burst rested on is not.
   Continuous fire overheats in 1 / (0.016 x 10) = 6.25 s, then waits
   (1.0 - 0.4) / 0.15 = 4 s. */
#define GAME_MG_HEAT_PER_SHOT 0.016f
#define GAME_MG_HEAT_COOL     0.12f /* per second, ready */
#define GAME_MG_HEAT_COOL_HOT 0.15f /* per second, overheated */
#define GAME_MG_HEAT_RESUME   0.4f

#endif
