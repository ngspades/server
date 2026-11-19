// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAPDOWNLOAD_H
#define MAPDOWNLOAD_H

/*
 * Maps the server does not have, fetched from the public maps repository
 * (github.com/ngspades/maps, raw.githubusercontent.com only): the
 * server-only archive ships no maps, so a host unzips it, starts it, and the
 * rotation's maps arrive by themselves.
 *
 * At start-up (Main.c, before the first map loads and before a mode's
 * map_fits reads a map's toml) each map the rotation names that is not
 * installed is downloaded synchronously, one progress line a map. Later, a
 * /map naming an uninstalled map starts one download on a thread of its
 * own (never on the tick) and the admin runs /map again once it is there.
 *
 * The repository keeps each map as <Name>.ngmap (quality 100: the geometry
 * exact, colours within a step, a seventh of the VXL's bytes): the ngmap is
 * fetched first and decoded into <Name>.vxl, the only format map_load reads;
 * a .vxl is fetched when there is no ngmap. A map folder the host copied
 * from the repository (an ngmap, no VXL) is unpacked the same way, beside
 * itself or in the download folder, with no network.
 *
 * Each file goes to a temporary file, is checked (the toml parses and has
 * [map] and [spawnpoints], the vxl's columns walk to its end for the size
 * the toml gives) and only then renamed into place, the .vxl last, since
 * map_file_path looks for it. Nothing downloaded is ever run: a map's .txt
 * is read by MapMeta.c, which evaluates literals and never runs Python.
 *
 * Where: Resources/maps/<Name>/ when it can be written, else the data
 * folder's maps/<Name>/ (--data: the macOS app's root is read only), which
 * map_file_path searches too.
 *
 * One map has many names: the repository's folder is 2Fort_CTF, aos.party
 * titles it "2Fort CTF" and a 0.75 server's file is 2fort.vxl. A name that
 * is not installed is resolved before it is downloaded, by a local folder
 * whose name normalises the same (lower case, letters and digits only) or by
 * the repository's aliases.json (tools/map_aliases.py), kept in the data
 * folder as maps/aliases.json and fetched again when older than a day.
 *
 * [maps] download = true|false in the config (both profiles; default true),
 * SP_MAP_DOWNLOAD=0|1 wins (the test tools set 0: they play the repository's
 * maps and must never reach the network for one). SP_MAPS_URL replaces the
 * repository's root for a test (https, or http on 127.0.0.1 only).
 */

#include <tomlc99/toml.h>

/* the longest map name (the maps repository's); every buffer that holds one
   is MAP_NAME_MAX + 1: 20 cut Beaulieu_Abbey_Zombie in the server list and
   made /map and game.set_map load a name that does not exist */
#define MAP_NAME_MAX 64

/* reads [maps] download (and the environment); call once the config is parsed */
void map_download_configure(toml_table_t* root);
int  map_download_enabled(void);
/* the .vxl and the .toml are both where map_file_path looks */
int map_installed(const char* name);
/* a name the repository could hold: letters, digits, '_', '-', '.', not a path */
int map_name_ok(const char* name);
/* the folder `name` means when it is not installed: a local folder whose
   name normalises the same, else the repository's aliases.json. Writes the
   folder into out and logs the resolution: 1 when it found one. may_fetch 0
   keeps it off the network (the tick); the background download passes 1 */
int map_resolve_alias(const char* name, char* out, size_t size, int may_fetch);
/* downloads a map now (start-up only: it blocks). 1 when it is installed afterwards */
int map_download_now(const char* name);
/* starts a download on its own thread: 1 started, 0 one is already running
   or downloads are off */
int map_download_background(const char* name);
/* the name being downloaded in the background, or "" */
const char* map_download_pending(void);

#endif
