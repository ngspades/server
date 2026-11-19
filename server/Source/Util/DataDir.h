// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DATADIR_H
#define DATADIR_H

#include <stddef.h>

/* Where the server writes its own files (Bans.json, master.key, webrtc.pem,
   logs/, the plugins' data). By default the data root it runs in, as
   always. `--data <dir>` (or SP_DATA_DIR) moves them to a folder of their
   own: a server inside a signed macOS .app runs from a read-only root (the
   bundle), and in the App Sandbox only its container is writable. Reads of
   the shipped data (Resources/config.toml, the maps) stay in the root. */

/* sets the folder (made when missing); NULL or "" keeps the root. Returns 0
   when the folder can be used */
int data_dir_set(const char* dir);

/* "" or the folder with a trailing '/' */
const char* data_dir(void);

/* the folder + name into out, and out returned: a name relative to the
   root when no folder was set */
const char* data_path(char* out, size_t size, const char* name);

/* Bans.json in the folder: read and written from many places */
const char* data_bans_file(void);

#endif
