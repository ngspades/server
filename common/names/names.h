// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/* Generated player names, shared by the client (a name for a fresh install,
   the Settings button) and the server (bot names, "Bot_" + a name), so both
   draw from the same words. Header only. */

#ifndef GAME_NAMES_H
#define GAME_NAMES_H

#include <stdio.h>
#include <string.h>

/* at most 7 + 8 letters, so every pair fits the 15 characters of a player
   name */
static const char* const game_name_adjectives[] = {
	"Swift", "Iron", "Silent", "Brave", "Rusty", "Wild", "Lucky", "Grim", "Bold", "Sly",
	"Mighty", "Steel", "Frosty", "Crimson", "Dusty", "Quick", "Rogue", "Stone", "Royal", "Shadow",
};
static const char* const game_name_nouns[] = {
	"Fox", "Badger", "Wolf", "Falcon", "Bear", "Hawk", "Viper", "Otter", "Raven", "Lynx",
	"Moose", "Beaver", "Cobra", "Panther", "Rhino", "Shark", "Mole", "Gopher", "Bison", "Spade",
};

#define GAME_NAME_ADJECTIVES (sizeof(game_name_adjectives) / sizeof(*game_name_adjectives))
#define GAME_NAME_NOUNS (sizeof(game_name_nouns) / sizeof(*game_name_nouns))

/* prefix + adjective + noun picked by two random numbers; when that is
   longer than max_len, the noun alone after the prefix */
static inline void game_name_make(char* out, size_t size, const char* prefix, unsigned a, unsigned n, size_t max_len) {
	const char* adj = game_name_adjectives[a % GAME_NAME_ADJECTIVES];
	const char* noun = game_name_nouns[n % GAME_NAME_NOUNS];
	if(strlen(prefix) + strlen(adj) + strlen(noun) <= max_len)
		snprintf(out, size, "%s%s%s", prefix, adj, noun);
	else
		snprintf(out, size, "%s%s", prefix, noun);
}

#endif
