// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NAMES_H
#define SERVER_NAMES_H

/*
 * Player names: one canonical spelling and one comparison skeleton.
 *
 * A name is what the other players read in the scoreboard and in chat, so it
 * must not be able to impersonate a player or a member of staff. The server
 * used to compare raw bytes case-sensitively: "Deuce" and "deuce" could sit in
 * the same match, and a zero-width character made "Admin" look like another
 * account. names_apply() strips control, zero-width and bidi characters,
 * folds the usual lookalikes into one skeleton and keeps the name unique
 * against every connected player (bots included).
 *
 * The player's name stays displayable (UTF-8 is kept); only the skeleton is
 * used for the comparisons. A changed name is logged as EVT name.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stddef.h>
#include <stdint.h>

/* Writes the sanitized, unique name into out (size bytes) and returns 1 when
 * it differs from raw. The player's id is used for the suffix that breaks a
 * collision. Safe with raw == NULL (an empty name becomes "Deuce"). */
uint8_t names_apply(server_t* server, player_t* player, const char* raw, char* out, size_t size);

/* The comparison skeleton: lower case, confusables folded, non-ASCII kept
 * as-is when it has no Latin lookalike. Exposed for the tests. */
void names_skeleton(const char* name, char* out, size_t size);

#endif
