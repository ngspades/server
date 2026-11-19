// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_CONFIGCHECK_H
#define SERVER_CONFIGCHECK_H

/*
 * --check-config: read the whole config without starting a server and report
 * every value that would be silently clamped, ignored or fatal later (an
 * unknown top-level table, a missing map, a port outside 1..65535, a bad
 * rotation name, a negative AFK timer). It does not need a map to load.
 * Every key the running profile does not read is a warning, the same line the
 * start-up logs (config_keys_log, ConfigKeys.h).
 *
 * Returns the number of errors (0 is good) and writes one line per problem to
 * out. Warnings (a plugin that is not downloaded yet) are reported too but do
 * not count.
 */

#include <Server/ConfigKeys.h>
#include <stddef.h>

int config_check(const char* path, char* out, size_t size);

#endif
